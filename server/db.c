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
int db_user_id(const char *user) {
    char eu[128]; esc(user, eu, sizeof(eu));
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT id FROM users WHERE username='%s'", eu);
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

int db_register(const char *user, const char *pass) {
    char hash[64]; sha1_hex(pass, hash);
    char eu[128]; esc(user, eu, sizeof(eu));
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT INTO users(username,password) VALUES('%s','%s')", eu, hash);
    LOCK();
    int rc;
    if (mysql_query(g_conn, sql) != 0) {
        rc = (mysql_errno(g_conn) == 1062) ? -R_USER_EXIST : -R_FAIL;
        UNLOCK(); return rc;
    }
    rc = (int)mysql_insert_id(g_conn);
    UNLOCK();
    return rc;
}

int db_login(const char *user, const char *pass) {
    char hash[64]; sha1_hex(pass, hash);
    char eu[128]; esc(user, eu, sizeof(eu));
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT id FROM users WHERE username='%s' AND password='%s'", eu, hash);
    LOCK();
    int id = -R_AUTH_FAIL;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        if (r && (row = mysql_fetch_row(r))) id = atoi(row[0]);
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return id;
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

int db_friend_list(int uid, char *out, int outsz,
                   int (*is_online)(int)) {
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT u.id,u.username,f.status FROM friends f "
        "JOIN users u ON u.id=f.friend_id WHERE f.user_id=%d", uid);
    LOCK();
    out[0] = 0;
    int used = 0;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        while (r && (row = mysql_fetch_row(r))) {
            int fid    = atoi(row[0]);
            int black  = atoi(row[2]);
            int online = is_online ? is_online(fid) : 0;
            int n = snprintf(out + used, outsz - used,
                             "%s\t%d\t%d\n", row[1], online, black);
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
        "SELECT m.from_id,m.target_id,m.msg_type,m.content,m.sent_at,u.username "
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
        "SELECT m.from_id,m.target_id,m.msg_type,m.content,m.sent_at,u.username "
        "FROM messages m JOIN users u ON u.id=m.from_id "
        "WHERE m.msg_type=0 AND ((m.from_id=%d AND m.target_id=%d) OR (m.from_id=%d AND m.target_id=%d)) "
        "ORDER BY m.id ASC LIMIT %d", a, b, b, a, max);
    return fetch_rows(sql, rows, max);
}

int db_history_group(int gid, OfflineRow *rows, int max) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT m.from_id,m.target_id,m.msg_type,m.content,m.sent_at,u.username "
        "FROM messages m JOIN users u ON u.id=m.from_id "
        "WHERE m.msg_type=1 AND m.target_id=%d ORDER BY m.id ASC LIMIT %d", gid, max);
    return fetch_rows(sql, rows, max);
}
