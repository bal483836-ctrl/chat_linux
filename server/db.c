#include "db.h"
#include <mysql/mysql.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <openssl/sha.h>

static MYSQL *g_conn = NULL;
/* MySQL 单连接不是线程安全: 加全局互斥锁串行化所有 SQL.
 * 教学项目这样足够; 生产环境应使用连接池. */
static pthread_mutex_t g_dbmu = PTHREAD_MUTEX_INITIALIZER;

#define LOCK()   pthread_mutex_lock(&g_dbmu)
#define UNLOCK() pthread_mutex_unlock(&g_dbmu)

/* ---- 工具: sha1 (40 hex) ---- */
static void sha1_hex(const char *in, char *out_hex) {
    unsigned char d[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char *)in, strlen(in), d);
    for (int i = 0; i < SHA_DIGEST_LENGTH; ++i) sprintf(out_hex + i * 2, "%02x", d[i]);
    out_hex[SHA_DIGEST_LENGTH * 2] = 0;
}

/* ---- 工具: 防注入用 mysql_real_escape_string ---- */
static void esc(const char *in, char *out, int outsz) {
    mysql_real_escape_string(g_conn, out, in, strlen(in));
    (void)outsz;
}

int db_init(const char *host, const char *user, const char *pass, const char *dbname) {
    if (mysql_library_init(0, NULL, NULL)) return -1;
    g_conn = mysql_init(NULL);
    if (!g_conn) return -1;
    if (!mysql_real_connect(g_conn, host, user, pass, dbname, 0, NULL, 0)) {
        fprintf(stderr, "MySQL connect failed: %s\n", mysql_error(g_conn));
        return -1;
    }
    mysql_set_character_set(g_conn, "utf8mb4");
    return 0;
}

void db_close(void) {
    if (g_conn) mysql_close(g_conn);
    mysql_library_end();
}

/* ===== 用户 ===== */
int db_user_id_by_account(const char *account) {
    int id = atoi(account) - ACCOUNT_BASE;
    if (id <= 0) return -1;
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT id FROM users WHERE id=%d", id);
    LOCK();
    int ok = -1;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        if (r && mysql_fetch_row(r)) ok = id;
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return ok;
}

/* 按昵称查 user_id. 昵称可重复, 这里返回第一条匹配; 主要用于查找
 * sql/init.sql 里的"小助手 / 新手指南"两个 mock 好友的 id. */
int db_user_id_by_nick(const char *nick) {
    char en[128]; esc(nick, en, sizeof(en));
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT id FROM users WHERE nickname='%s' ORDER BY id ASC LIMIT 1", en);
    LOCK();
    int id = -1;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        if (r && (row = mysql_fetch_row(r))) id = atoi(row[0]);
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return id;
}

int db_register(const char *nickname, const char *pass, const char *email) {
    char hash[64]; sha1_hex(pass, hash);
    char en[128]; esc(nickname, en, sizeof(en));
    char ee[160]; if (email && email[0]) esc(email, ee, sizeof(ee)); else ee[0] = 0;
    int color = 0;
    for (const char *p = nickname; *p; ++p) color = (color * 131 + (unsigned char)*p) & 0xFFFF;
    color %= 10;
    char sql[600];
    if (ee[0])
        snprintf(sql, sizeof(sql),
            "INSERT INTO users(nickname,password,avatar_color,email) VALUES('%s','%s',%d,'%s')",
            en, hash, color, ee);
    else
        snprintf(sql, sizeof(sql),
            "INSERT INTO users(nickname,password,avatar_color) VALUES('%s','%s',%d)",
            en, hash, color);
    LOCK();
    int rc;
    if (mysql_query(g_conn, sql) != 0) {
        rc = -RS_FAIL;
        UNLOCK(); return rc;
    }
    rc = (int)mysql_insert_id(g_conn);
    UNLOCK();
    return rc;
}

int db_email_exists(const char *email) {
    if (!email || !email[0]) return 0;
    char ee[160]; esc(email, ee, sizeof(ee));
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT id FROM users WHERE email='%s'", ee);
    LOCK();
    int yes = 0;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        if (r && mysql_fetch_row(r)) yes = 1;
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return yes;
}

int db_login_by_email(const char *email, const char *pass) {
    char hash[64]; sha1_hex(pass, hash);
    char ee[160]; esc(email, ee, sizeof(ee));
    char sql[320];
    snprintf(sql, sizeof(sql),
        "SELECT id FROM users WHERE email='%s' AND password='%s'", ee, hash);
    LOCK();
    int id = -RS_AUTH_FAIL;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        if (r && (row = mysql_fetch_row(r))) id = atoi(row[0]);
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return id;
}

int db_login_by_id(int uid, const char *pass) {
    char hash[64]; sha1_hex(pass, hash);
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT id FROM users WHERE id=%d AND password='%s'", uid, hash);
    LOCK();
    int id = -RS_AUTH_FAIL;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        if (r && (row = mysql_fetch_row(r))) id = atoi(row[0]);
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return id;
}

int db_get_nick(int uid, char *out, int outsz) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT nickname FROM users WHERE id=%d", uid);
    LOCK();
    int ok = -1;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        if (r && (row = mysql_fetch_row(r))) {
            strncpy(out, row[0], outsz - 1); out[outsz - 1] = 0; ok = 0;
        }
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return ok;
}

int db_get_avatar_color(int uid) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT avatar_color FROM users WHERE id=%d", uid);
    LOCK();
    int c = 0;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        if (r && (row = mysql_fetch_row(r))) c = atoi(row[0]);
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return c;
}

int db_set_online(int uid, int on) {
    char sql[128];
    snprintf(sql, sizeof(sql), "UPDATE users SET online=%d WHERE id=%d", on?1:0, uid);
    LOCK(); int rc = mysql_query(g_conn, sql); UNLOCK();
    return rc == 0 ? 0 : -1;
}

/* ===== 好友 ===== */
int db_friend_add(int uid, int fid) {
    if (uid == fid) return -1;
    char sql[256];
    /* 双向插入, 便于反向查询 */
    snprintf(sql, sizeof(sql),
        "INSERT IGNORE INTO friends(user_id,friend_id,status) VALUES(%d,%d,0),(%d,%d,0)",
        uid, fid, fid, uid);
    LOCK(); int rc = mysql_query(g_conn, sql); UNLOCK();
    return rc == 0 ? 0 : -1;
}

int db_friend_del(int uid, int fid) {
    char sql[256];
    snprintf(sql, sizeof(sql),
        "DELETE FROM friends WHERE (user_id=%d AND friend_id=%d) OR (user_id=%d AND friend_id=%d)",
        uid, fid, fid, uid);
    LOCK(); int rc = mysql_query(g_conn, sql); UNLOCK();
    return rc == 0 ? 0 : -1;
}

int db_black_set(int uid, int fid, int black) {
    char sql[256];
    /* 单向: uid 是否拉黑 fid */
    snprintf(sql, sizeof(sql),
        "INSERT INTO friends(user_id,friend_id,status) VALUES(%d,%d,%d) "
        "ON DUPLICATE KEY UPDATE status=%d",
        uid, fid, black?1:0, black?1:0);
    LOCK(); int rc = mysql_query(g_conn, sql); UNLOCK();
    return rc == 0 ? 0 : -1;
}

int db_is_friend(int uid, int fid) {
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT status FROM friends WHERE user_id=%d AND friend_id=%d", uid, fid);
    LOCK();
    int yes = 0;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        if (r && mysql_fetch_row(r)) yes = 1;
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return yes;
}

int db_is_black(int uid, int fid) {
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT status FROM friends WHERE user_id=%d AND friend_id=%d AND status=1",
        uid, fid);
    LOCK();
    int yes = 0;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        if (r && mysql_fetch_row(r)) yes = 1;
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return yes;
}

/* 输出格式: "account\tnickname\tavatar_color\tonline\tblack\tremark\n" */
int db_friend_list(int uid, char *out, int outsz,
                   int (*is_online)(int)) {
    char sql[320];
    snprintf(sql, sizeof(sql),
        "SELECT u.id,u.nickname,u.avatar_color,f.status,f.remark FROM friends f "
        "JOIN users u ON u.id=f.friend_id WHERE f.user_id=%d", uid);
    LOCK();
    out[0] = 0;
    int used = 0;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        while (r && (row = mysql_fetch_row(r))) {
            int fid    = atoi(row[0]);
            int color  = atoi(row[2]);
            int black  = atoi(row[3]);
            int online = is_online ? is_online(fid) : 0;
            const char *remark = row[4] ? row[4] : "";
            int n = snprintf(out + used, outsz - used,
                             "%d\t%s\t%d\t%d\t%d\t%s\n",
                             ACCOUNT_BASE + fid, row[1], color, online, black, remark);
            if (n <= 0 || n >= outsz - used) break;
            used += n;
        }
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return used;
}

/* ===== 群组 ===== */
int db_group_create(int owner, const char *name) {
    char en[128]; esc(name, en, sizeof(en));
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT INTO chat_groups(name,owner_id) VALUES('%s',%d)", en, owner);
    LOCK();
    int gid = -1;
    if (mysql_query(g_conn, sql) == 0) {
        gid = (int)mysql_insert_id(g_conn);
        snprintf(sql, sizeof(sql),
            "INSERT INTO group_members(group_id,user_id) VALUES(%d,%d)", gid, owner);
        mysql_query(g_conn, sql);
    }
    UNLOCK();
    return gid;
}

int db_group_join(int gid, int uid) {
    char sql[256];
    snprintf(sql, sizeof(sql),
        "INSERT IGNORE INTO group_members(group_id,user_id) VALUES(%d,%d)", gid, uid);
    LOCK(); int rc = mysql_query(g_conn, sql); UNLOCK();
    return rc == 0 ? 0 : -1;
}

int db_group_list_for_user(int uid, char *out, int outsz) {
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT g.id,g.name,g.owner_id FROM chat_groups g "
        "JOIN group_members m ON m.group_id=g.id WHERE m.user_id=%d", uid);
    LOCK();
    out[0] = 0; int used = 0;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        while (r && (row = mysql_fetch_row(r))) {
            int n = snprintf(out + used, outsz - used,
                             "%s\t%s\t%s\n", row[0], row[1], row[2]);
            if (n <= 0 || n >= outsz - used) break;
            used += n;
        }
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return used;
}

int db_group_members(int gid, int *ids, int max) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT user_id FROM group_members WHERE group_id=%d", gid);
    LOCK();
    int n = 0;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        while (r && n < max && (row = mysql_fetch_row(r))) ids[n++] = atoi(row[0]);
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return n;
}

/* ===== 消息 ===== */
int db_save_msg(int from, int target, int type, const char *content) {
    char ec[MAX_BODY_LEN * 2 + 4];
    mysql_real_escape_string(g_conn, ec, content, strlen(content));
    char *sql = (char *)malloc(strlen(ec) + 256);
    sprintf(sql,
        "INSERT INTO messages(from_id,target_id,msg_type,content) VALUES(%d,%d,%d,'%s')",
        from, target, type, ec);
    LOCK();
    int id = -1;
    if (mysql_query(g_conn, sql) == 0) id = (int)mysql_insert_id(g_conn);
    UNLOCK();
    free(sql);
    return id;
}

int db_offline_put(int uid, int mid) {
    char sql[128];
    snprintf(sql, sizeof(sql),
        "INSERT INTO offline_msg(user_id,message_id) VALUES(%d,%d)", uid, mid);
    LOCK(); int rc = mysql_query(g_conn, sql); UNLOCK();
    return rc == 0 ? 0 : -1;
}

static int fetch_rows(const char *sql, OfflineRow *rows, int max) {
    LOCK();
    int n = 0;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        while (r && n < max && (row = mysql_fetch_row(r))) {
            rows[n].from_id   = atoi(row[0]);
            rows[n].target_id = atoi(row[1]);
            rows[n].msg_type  = atoi(row[2]);
            strncpy(rows[n].content,   row[3] ? row[3] : "", MAX_BODY_LEN - 1);
            strncpy(rows[n].sent_at,   row[4] ? row[4] : "", 31);
            strncpy(rows[n].from_name, row[5] ? row[5] : "", MAX_NAME_LEN - 1);
            n++;
        }
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return n;
}

int db_offline_take(int uid, OfflineRow *rows, int max) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT m.from_id,m.target_id,m.msg_type,m.content,m.sent_at,u.nickname "
        "FROM offline_msg o JOIN messages m ON m.id=o.message_id "
        "JOIN users u ON u.id=m.from_id "
        "WHERE o.user_id=%d ORDER BY m.id ASC LIMIT %d", uid, max);
    int n = fetch_rows(sql, rows, max);
    /* 取走后删除 */
    char del[128];
    snprintf(del, sizeof(del), "DELETE FROM offline_msg WHERE user_id=%d", uid);
    LOCK(); mysql_query(g_conn, del); UNLOCK();
    return n;
}

int db_history_priv(int a, int b, OfflineRow *rows, int max) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT m.from_id,m.target_id,m.msg_type,m.content,m.sent_at,u.nickname "
        "FROM messages m JOIN users u ON u.id=m.from_id "
        "WHERE m.msg_type=0 AND ((m.from_id=%d AND m.target_id=%d) OR (m.from_id=%d AND m.target_id=%d)) "
        "ORDER BY m.id ASC LIMIT %d", a, b, b, a, max);
    return fetch_rows(sql, rows, max);
}

int db_history_group(int gid, OfflineRow *rows, int max) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT m.from_id,m.target_id,m.msg_type,m.content,m.sent_at,u.nickname "
        "FROM messages m JOIN users u ON u.id=m.from_id "
        "WHERE m.msg_type=1 AND m.target_id=%d ORDER BY m.id ASC LIMIT %d", gid, max);
    return fetch_rows(sql, rows, max);
}

/* ===== 好友申请 ===== */
int db_freq_put(int from_id, int to_id, const char *hello) {
    char eh[512] = "";
    if (hello && *hello) mysql_real_escape_string(g_conn, eh, hello, strlen(hello));
    char sql[1024];
    LOCK();
    /* 已存在 pending → 复用, 更新 hello */
    snprintf(sql, sizeof(sql),
        "SELECT id FROM friend_requests WHERE from_id=%d AND to_id=%d AND status=0",
        from_id, to_id);
    int reqid = -1;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        if (r && (row = mysql_fetch_row(r))) reqid = atoi(row[0]);
        if (r) mysql_free_result(r);
    }
    if (reqid > 0) {
        snprintf(sql, sizeof(sql),
            "UPDATE friend_requests SET hello='%s' WHERE id=%d", eh, reqid);
        mysql_query(g_conn, sql);
    } else {
        snprintf(sql, sizeof(sql),
            "INSERT INTO friend_requests(from_id,to_id,hello,status) VALUES(%d,%d,'%s',0)",
            from_id, to_id, eh);
        if (mysql_query(g_conn, sql) == 0) reqid = (int)mysql_insert_id(g_conn);
    }
    UNLOCK();
    return reqid;
}

int db_freq_info(int reqid, int *from_id, int *to_id) {
    char sql[128];
    snprintf(sql, sizeof(sql),
        "SELECT from_id,to_id FROM friend_requests WHERE id=%d", reqid);
    LOCK();
    int ok = -1;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        if (r && (row = mysql_fetch_row(r))) {
            *from_id = atoi(row[0]); *to_id = atoi(row[1]); ok = 0;
        }
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return ok;
}

int db_freq_set(int reqid, int status) {
    char sql[128];
    snprintf(sql, sizeof(sql),
        "UPDATE friend_requests SET status=%d WHERE id=%d", status, reqid);
    LOCK(); int rc = mysql_query(g_conn, sql); UNLOCK();
    return rc == 0 ? 0 : -1;
}

/* "reqid\tfrom_account\tfrom_nick\tavatar_color\ttime\thello\n" */
int db_freq_list(int to_id, char *out, int outsz) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT fr.id,u.id,u.nickname,u.avatar_color,fr.created_at,fr.hello "
        "FROM friend_requests fr JOIN users u ON u.id=fr.from_id "
        "WHERE fr.to_id=%d AND fr.status=0 ORDER BY fr.id DESC", to_id);
    LOCK();
    out[0] = 0; int used = 0;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        while (r && (row = mysql_fetch_row(r))) {
            int n = snprintf(out + used, outsz - used,
                             "%s\t%d\t%s\t%s\t%s\t%s\n",
                             row[0], atoi(row[1]) + ACCOUNT_BASE,
                             row[2], row[3], row[4], row[5] ? row[5] : "");
            if (n <= 0 || n >= outsz - used) break;
            used += n;
        }
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return used;
}

/* ===== 入群申请 ===== */
int db_greq_put(int gid, int user_id, const char *hello) {
    char eh[512] = "";
    if (hello && *hello) mysql_real_escape_string(g_conn, eh, hello, strlen(hello));
    char sql[1024];
    LOCK();
    snprintf(sql, sizeof(sql),
        "SELECT id FROM group_join_requests WHERE group_id=%d AND user_id=%d AND status=0",
        gid, user_id);
    int reqid = -1;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        if (r && (row = mysql_fetch_row(r))) reqid = atoi(row[0]);
        if (r) mysql_free_result(r);
    }
    if (reqid > 0) {
        snprintf(sql, sizeof(sql),
            "UPDATE group_join_requests SET hello='%s' WHERE id=%d", eh, reqid);
        mysql_query(g_conn, sql);
    } else {
        snprintf(sql, sizeof(sql),
            "INSERT INTO group_join_requests(group_id,user_id,hello,status) VALUES(%d,%d,'%s',0)",
            gid, user_id, eh);
        if (mysql_query(g_conn, sql) == 0) reqid = (int)mysql_insert_id(g_conn);
    }
    UNLOCK();
    return reqid;
}

int db_greq_info(int reqid, int *gid, int *user_id) {
    char sql[128];
    snprintf(sql, sizeof(sql),
        "SELECT group_id,user_id FROM group_join_requests WHERE id=%d", reqid);
    LOCK();
    int ok = -1;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        if (r && (row = mysql_fetch_row(r))) {
            *gid = atoi(row[0]); *user_id = atoi(row[1]); ok = 0;
        }
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return ok;
}

int db_greq_set(int reqid, int status) {
    char sql[128];
    snprintf(sql, sizeof(sql),
        "UPDATE group_join_requests SET status=%d WHERE id=%d", status, reqid);
    LOCK(); int rc = mysql_query(g_conn, sql); UNLOCK();
    return rc == 0 ? 0 : -1;
}

int db_greq_list_for_owner(int owner_id, char *out, int outsz) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT gr.id,g.id,g.name,u.nickname,gr.created_at,gr.hello "
        "FROM group_join_requests gr "
        "JOIN chat_groups g ON g.id=gr.group_id "
        "JOIN users u ON u.id=gr.user_id "
        "WHERE g.owner_id=%d AND gr.status=0 ORDER BY gr.id DESC", owner_id);
    LOCK();
    out[0] = 0; int used = 0;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        while (r && (row = mysql_fetch_row(r))) {
            int n = snprintf(out + used, outsz - used,
                "%s\t%s\t%s\t%s\t%s\t%s\n",
                row[0], row[1], row[2], row[3], row[4], row[5] ? row[5] : "");
            if (n <= 0 || n >= outsz - used) break;
            used += n;
        }
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return used;
}

int db_group_owner(int gid) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT owner_id FROM chat_groups WHERE id=%d", gid);
    LOCK();
    int id = -1;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        if (r && (row = mysql_fetch_row(r))) id = atoi(row[0]);
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return id;
}

int db_group_name(int gid, char *out, int outsz) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT name FROM chat_groups WHERE id=%d", gid);
    LOCK();
    int ok = -1;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        if (r && (row = mysql_fetch_row(r))) {
            strncpy(out, row[0], outsz - 1); out[outsz - 1] = 0; ok = 0;
        }
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return ok;
}

int db_group_member_count(int gid) {
    char sql[128];
    snprintf(sql, sizeof(sql),
        "SELECT COUNT(*) FROM group_members WHERE group_id=%d", gid);
    LOCK();
    int n = 0;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        if (r && (row = mysql_fetch_row(r))) n = atoi(row[0]);
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return n;
}

/* 搜索: 关键字若是 6+ 位数字解释为账号精确匹配, 否则按昵称模糊匹配.
 * 输出格式: "account\tnickname\tavatar_color\tonline\n" */
int db_user_search(const char *q, char *out, int outsz,
                   int (*is_online)(int)) {
    char eq[256]; mysql_real_escape_string(g_conn, eq, q, strlen(q));
    int as_account = 0;
    if (strlen(q) >= 6) {
        as_account = 1;
        for (const char *p = q; *p; ++p) if (*p < '0' || *p > '9') { as_account = 0; break; }
    }
    char sql[512];
    if (as_account) {
        int id = atoi(q) - ACCOUNT_BASE;
        snprintf(sql, sizeof(sql),
            "SELECT id,nickname,avatar_color FROM users WHERE id=%d", id);
    } else {
        snprintf(sql, sizeof(sql),
            "SELECT id,nickname,avatar_color FROM users "
            "WHERE nickname LIKE '%%%s%%' LIMIT 50", eq);
    }
    LOCK();
    out[0] = 0; int used = 0;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        while (r && (row = mysql_fetch_row(r))) {
            int uid    = atoi(row[0]);
            int color  = atoi(row[2]);
            int online = is_online ? is_online(uid) : 0;
            int n = snprintf(out + used, outsz - used,
                             "%d\t%s\t%d\t%d\n",
                             ACCOUNT_BASE + uid, row[1], color, online);
            if (n <= 0 || n >= outsz - used) break;
            used += n;
        }
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return used;
}

int db_group_search(const char *q, char *out, int outsz) {
    char eq[256]; mysql_real_escape_string(g_conn, eq, q, strlen(q));
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT g.id,g.name,u.nickname,COUNT(m.user_id) "
        "FROM chat_groups g "
        "JOIN users u ON u.id=g.owner_id "
        "LEFT JOIN group_members m ON m.group_id=g.id "
        "WHERE g.name LIKE '%%%s%%' "
        "GROUP BY g.id LIMIT 50", eq);
    LOCK();
    out[0] = 0; int used = 0;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        while (r && (row = mysql_fetch_row(r))) {
            int n = snprintf(out + used, outsz - used,
                             "%s\t%s\t%s\t%s\n",
                             row[0], row[1], row[2], row[3]);
            if (n <= 0 || n >= outsz - used) break;
            used += n;
        }
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return used;
}

/* 消息全文搜索.
 *
 * SQL 思路: 把"我参与的消息"分两路 UNION:
 *   1) 私聊 (msg_type=0): from_id=我 或 target_id=我
 *   2) 群聊 (msg_type=1): target_id 属于我加入的群
 * 然后再 WHERE content LIKE '%关键字%', ORDER BY sent_at DESC LIMIT 50.
 *
 * 注意拼 SQL 时关键字已用 mysql_real_escape_string 防注入; LIKE 的
 * %% 是给 printf 转义, 实际下到 MySQL 是单 %.
 *
 * 输出每行: "msg_id\tsent_at\tfrom_nick\tkind\tpeer\tsnippet"
 *   - kind 私聊=0 群聊=1
 *   - peer 私聊填对方账号 (=ACCOUNT_BASE+对方id), 群聊填群号
 *   - snippet 取 content 前 80 字符截断, 制表符替换成空格, 避免破坏分隔 */
int db_msg_search(int user_id, const char *q, char *out, int outsz) {
    char eq[256]; mysql_real_escape_string(g_conn, eq, q, strlen(q));
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "SELECT m.id, m.sent_at, u.nickname, m.msg_type, "
        "       CASE WHEN m.msg_type=0 "
        "            THEN IF(m.from_id=%d, m.target_id, m.from_id) "
        "            ELSE m.target_id END AS peer_raw, "
        "       LEFT(m.content, 80) "
        "FROM messages m "
        "JOIN users u ON u.id = m.from_id "
        "WHERE m.content LIKE '%%%s%%' AND ("
        "       (m.msg_type=0 AND (m.from_id=%d OR m.target_id=%d)) "
        "    OR (m.msg_type=1 AND m.target_id IN ("
        "          SELECT group_id FROM group_members WHERE user_id=%d))"
        ") "
        "ORDER BY m.sent_at DESC LIMIT 50",
        user_id, eq, user_id, user_id, user_id);
    LOCK();
    out[0] = 0; int used = 0;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        while (r && (row = mysql_fetch_row(r))) {
            int kind = atoi(row[3]);
            int peer_raw = atoi(row[4]);
            int peer_display = (kind == 0) ? (peer_raw + ACCOUNT_BASE) : peer_raw;
            /* snippet 里的 \t \n 替换成空格, 避免冲掉行/列分隔 */
            char snippet[128] = {0};
            if (row[5]) {
                strncpy(snippet, row[5], sizeof(snippet) - 1);
                for (char *p = snippet; *p; ++p) if (*p == '\t' || *p == '\n') *p = ' ';
            }
            int n = snprintf(out + used, outsz - used,
                             "%s\t%s\t%s\t%d\t%d\t%s\n",
                             row[0], row[1], row[2] ? row[2] : "?",
                             kind, peer_display, snippet);
            if (n <= 0 || n >= outsz - used) break;
            used += n;
        }
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return used;
}

/* ============================================================
 *  好友备注 / 群成员管理 / 群公告 / 个人资料  (Web 原型新增功能)
 * ============================================================ */

int db_friend_set_remark(int uid, int fid, const char *remark) {
    char en[128]; esc(remark, en, sizeof(en));
    char sql[256];
    snprintf(sql, sizeof(sql),
        "UPDATE friends SET remark='%s' WHERE user_id=%d AND friend_id=%d", en, uid, fid);
    LOCK(); int rc = mysql_query(g_conn, sql) ? -1 : 0; UNLOCK();
    return rc;
}

int db_group_is_member(int gid, int uid) {
    char sql[160];
    snprintf(sql, sizeof(sql),
        "SELECT 1 FROM group_members WHERE group_id=%d AND user_id=%d", gid, uid);
    LOCK();
    int yes = 0;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        if (r && mysql_fetch_row(r)) yes = 1;
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return yes;
}

int db_group_add_member(int gid, int uid) {
    char sql[160];
    snprintf(sql, sizeof(sql),
        "INSERT IGNORE INTO group_members(group_id,user_id) VALUES(%d,%d)", gid, uid);
    LOCK(); int rc = mysql_query(g_conn, sql) ? -1 : 0; UNLOCK();
    return rc;
}

int db_group_leave(int gid, int uid) {
    char sql[160];
    snprintf(sql, sizeof(sql),
        "DELETE FROM group_members WHERE group_id=%d AND user_id=%d", gid, uid);
    LOCK(); int rc = mysql_query(g_conn, sql) ? -1 : 0; UNLOCK();
    return rc;
}

int db_group_notice_get(int gid, char *out, int outsz) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT notice FROM chat_groups WHERE id=%d", gid);
    LOCK();
    int rc = -1; out[0] = 0;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        if (r && (row = mysql_fetch_row(r))) {
            strncpy(out, row[0] ? row[0] : "", outsz - 1);
            out[outsz - 1] = 0; rc = 0;
        }
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return rc;
}

int db_group_set_notice(int gid, const char *notice) {
    char en[1100]; esc(notice, en, sizeof(en));
    char sql[1300];
    snprintf(sql, sizeof(sql), "UPDATE chat_groups SET notice='%s' WHERE id=%d", en, gid);
    LOCK(); int rc = mysql_query(g_conn, sql) ? -1 : 0; UNLOCK();
    return rc;
}

int db_set_birthday(int uid, const char *birth) {
    char sql[160];
    if (birth && birth[0])
        snprintf(sql, sizeof(sql), "UPDATE users SET birthday='%.10s' WHERE id=%d", birth, uid);
    else
        snprintf(sql, sizeof(sql), "UPDATE users SET birthday=NULL WHERE id=%d", uid);
    LOCK(); int rc = mysql_query(g_conn, sql) ? -1 : 0; UNLOCK();
    return rc;
}

int db_set_nick(int uid, const char *nick) {
    char en[128]; esc(nick, en, sizeof(en));
    char sql[256];
    snprintf(sql, sizeof(sql), "UPDATE users SET nickname='%s' WHERE id=%d", en, uid);
    LOCK(); int rc = mysql_query(g_conn, sql) ? -1 : 0; UNLOCK();
    return rc;
}

int db_profile_get(int uid, char *nick, int nsz, char *birth, int bsz, int *color) {
    char sql[200];
    snprintf(sql, sizeof(sql),
        "SELECT nickname,avatar_color,DATE_FORMAT(birthday,'%%Y-%%m-%%d') FROM users WHERE id=%d", uid);
    LOCK();
    int rc = -1;
    if (nick)  nick[0]  = 0;
    if (birth) birth[0] = 0;
    if (color) *color   = 0;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        if (r && (row = mysql_fetch_row(r))) {
            if (nick)  { strncpy(nick, row[0] ? row[0] : "", nsz - 1); nick[nsz - 1] = 0; }
            if (color) *color = atoi(row[1] ? row[1] : "0");
            if (birth) { strncpy(birth, row[2] ? row[2] : "", bsz - 1); birth[bsz - 1] = 0; }
            rc = 0;
        }
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return rc;
}
