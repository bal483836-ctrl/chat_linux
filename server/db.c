/* =========================================================
 *  db.c - 数据访问层 (DAO)
 *
 *  所有与 MySQL 的交互都收敛在本文件, 上层(handler.c)只调用
 *  db_xxx() 函数, 不直接碰 SQL。
 *
 *  线程模型:
 *    - 进程全局只维护一条 MySQL 连接 g_conn;
 *    - libmysqlclient 的单连接对象不是线程安全的, 因此用一把全局
 *      互斥锁 g_dbmu 把"每一次查询"串行化(见下面的 LOCK/UNLOCK)。
 *    - 每个 db_xxx() 内部都遵循同一套定式:
 *          拼 SQL -> LOCK() -> mysql_query() -> 取结果 -> free -> UNLOCK()
 *      读操作还要 mysql_store_result()/mysql_fetch_row()/mysql_free_result()。
 *
 *  安全:
 *    - 一切来自用户的字符串在拼进 SQL 前都要经 esc()/mysql_real_escape_string()
 *      转义, 防止 SQL 注入; 纯整数参数(id/gid)直接 %d 拼接是安全的。
 *
 *  返回值约定:
 *    - 多数写操作 0 成功 / -1 失败;
 *    - 插入类返回新行的自增 id(或负的错误码);
 *    - 列表类把结果拼成多行文本填进调用方给的 out 缓冲, 返回写入的字节数。
 * ========================================================= */
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

/* 所有 db_xxx() 在访问 g_conn 前后成对调用 LOCK()/UNLOCK() 串行化访问 */
#define LOCK()   pthread_mutex_lock(&g_dbmu)
#define UNLOCK() pthread_mutex_unlock(&g_dbmu)

/* ---- 工具: 把明文口令算成 SHA1, 输出 40 个十六进制字符(+结尾 0) ----
 * 数据库里只存这个摘要, 不存明文。注意: SHA1 无盐、且已不适合做口令
 * 存储(生产应换 bcrypt/argon2), 这里仅为教学演示够用。 */
static void sha1_hex(const char *in, char *out_hex) {
    unsigned char d[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char *)in, strlen(in), d);
    for (int i = 0; i < SHA_DIGEST_LENGTH; ++i) sprintf(out_hex + i * 2, "%02x", d[i]);
    out_hex[SHA_DIGEST_LENGTH * 2] = 0;
}

/* ---- 工具: 对字符串做 SQL 转义, 防注入 ----
 * 包装 mysql_real_escape_string; 调用方须保证 out 至少有 strlen(in)*2+1
 * 的空间(转义最坏情况每个字符变两个)。outsz 仅用于自文档, 未做校验。 */
static void esc(const char *in, char *out, int outsz) {
    mysql_real_escape_string(g_conn, out, in, strlen(in));
    (void)outsz;
}

/* 建立到 MySQL 的全局连接, 服务器启动时调用一次。返回 0 成功 / -1 失败。
 * 字符集设为 utf8mb4, 这样中文和 emoji(4 字节)都能正确存取。 */
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

/* 关闭连接, 释放库资源, 服务器退出时调用。 */
void db_close(void) {
    if (g_conn) mysql_close(g_conn);
    mysql_library_end();
}

/* ===== 用户 ===== */
/* 把客户端可见的账号字符串(如 "100001")换算成内部 user_id 并校验其存在。
 * 账号 = ACCOUNT_BASE + id, 所以先减去基数; 查库确认后返回 id, 否则 -1。 */
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

/* 注册新用户。口令先 SHA1, 昵称/邮箱先转义。
 * avatar_color 由昵称做一次简易多项式 hash(*131) 再 %10 得到 0..9 的默认
 * 头像色号; 用户之后若在客户端选了具体动物形象会用 db_set_avatar_color 覆盖。
 * 成功返回新用户的 id(即自增主键), 失败返回 -RS_FAIL。 */
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

/* 邮箱是否已被注册, 用于注册前查重。1=已存在, 0=不存在。 */
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

/* 邮箱 + 口令登录。命中返回 user_id, 否则返回 -RS_AUTH_FAIL。 */
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

/* 账号(id) + 口令登录。命中返回 user_id, 否则返回 -RS_AUTH_FAIL。 */
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

/* 取用户昵称填进 out。成功 0 / 失败 -1(用户不存在或查询失败)。 */
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

/* 取用户头像色号(0..9), 客户端据此还原一致的动物头像。查询失败返回 0。 */
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

/* 把 users.online 落库为 1/0。登录/掉线时调用, 供离线查询等使用。
 * (注意: 实时在线判断以内存表 online.c 为准, 这里的字段是持久化冗余。) */
int db_set_online(int uid, int on) {
    char sql[128];
    snprintf(sql, sizeof(sql), "UPDATE users SET online=%d WHERE id=%d", on?1:0, uid);
    LOCK(); int rc = mysql_query(g_conn, sql); UNLOCK();
    return rc == 0 ? 0 : -1;
}

/* ===== 好友 ===== */
/* 建立好友关系。friends 表存的是有向边(user_id -> friend_id), 这里一次插入
 * 两条(A->B 和 B->A)构成双向关系, 之后任一方查自己的好友列表都能查到对方。
 * INSERT IGNORE 保证重复添加不报错。不能加自己(uid==fid)。 */
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

/* 解除好友关系: 把双向的两条边都删掉。 */
int db_friend_del(int uid, int fid) {
    char sql[256];
    snprintf(sql, sizeof(sql),
        "DELETE FROM friends WHERE (user_id=%d AND friend_id=%d) OR (user_id=%d AND friend_id=%d)",
        uid, fid, fid, uid);
    LOCK(); int rc = mysql_query(g_conn, sql); UNLOCK();
    return rc == 0 ? 0 : -1;
}

/* 设置/取消拉黑。复用 friends 表的 status 列: 0=普通好友, 1=已拉黑。
 * 与好友关系不同, 拉黑是单向的(只改 uid->fid 这条边)。ON DUPLICATE KEY
 * 让"边已存在则更新 status, 不存在则插入"一步完成。 */
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

/* uid 是否已把 fid 当好友(存在这条边即算, 不管黑名单)。1/0。 */
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

/* uid 是否拉黑了 fid(status=1)。1/0。发私聊前用它拦截被对方拉黑的消息。 */
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

/* 列出 uid 的所有好友, 拼成多行文本填进 out, 返回写入字节数。
 * 每行: "account\tnickname\tavatar_color\tonline\tblack\tremark\n"。
 * 在线状态(online)由调用方传入的回调 is_online(fid) 实时判定(查内存在线表),
 * 而不是读 users.online 字段, 保证准确。
 * 循环里 snprintf 的返回值一旦 >= 剩余空间就 break, 避免写溢出/截断半行。 */
int db_friend_list(int uid, char *out, int outsz,
                   int (*is_online)(int)) {
    char sql[320];
    /* friends 有向边 join users 拿到好友的昵称/头像色/备注 */
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
            int black  = atoi(row[3]);   /* f.status: 1 表示我把这个好友拉黑了 */
            int online = is_online ? is_online(fid) : 0;
            const char *remark = row[4] ? row[4] : "";
            int n = snprintf(out + used, outsz - used,
                             "%d\t%s\t%d\t%d\t%d\t%s\n",
                             ACCOUNT_BASE + fid, row[1], color, online, black, remark);
            if (n <= 0 || n >= outsz - used) break;   /* 缓冲写满, 停止 */
            used += n;
        }
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return used;
}

/* ===== 群组 ===== */
/* 建群: 先插 chat_groups 拿到自增 gid, 再把群主本人加进 group_members
 * (创建者自动成为第一个成员)。两步在同一把锁内完成。返回 gid, 失败 -1。 */
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

/* 把 uid 加进群 gid(审批通过或兼容路径)。INSERT IGNORE 防重复入群。0/-1。 */
int db_group_join(int gid, int uid) {
    char sql[256];
    snprintf(sql, sizeof(sql),
        "INSERT IGNORE INTO group_members(group_id,user_id) VALUES(%d,%d)", gid, uid);
    LOCK(); int rc = mysql_query(g_conn, sql); UNLOCK();
    return rc == 0 ? 0 : -1;
}

/* 列出 uid 加入的所有群, 每行 "gid\tname\towner_id\n", 返回写入字节数。 */
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

/* 取群成员的 user_id 列表填进 ids[](最多 max 个), 返回实际个数。
 * 群聊分发消息时要遍历这个列表逐个在线推送/离线入队。 */
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
/* 把一条消息落库(不论在线离线, 所有消息都先入 messages 表, 才能支持历史/离线)。
 *   from   发送者 uid
 *   target 私聊=对方 uid, 群聊=gid
 *   type   0=私聊 1=群聊
 * content 可能含二进制/长文本, 故用堆上缓冲并 escape。返回新 msg_id, 失败 -1。 */
int db_save_msg(int from, int target, int type, const char *content) {
    char ec[MAX_BODY_LEN * 2 + 4];   /* 转义最坏膨胀一倍, 预留足够空间 */
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

/* 更新一条消息的 content(用于文件消息: 先存占位拿到 msgid, 再回填带 fileid 的标记). */
int db_update_msg_content(int msgid, const char *content) {
    char ec[MAX_BODY_LEN * 2 + 4];
    mysql_real_escape_string(g_conn, ec, content, strlen(content));
    char *sql = (char *)malloc(strlen(ec) + 128);
    sprintf(sql, "UPDATE messages SET content='%s' WHERE id=%d", ec, msgid);
    LOCK(); int rc = mysql_query(g_conn, sql) ? -1 : 0; UNLOCK();
    free(sql);
    return rc;
}

/* 按 msgid 取消息的发送者 id 与 content(下载文件时用来定位发送者/文件元信息). 0/-1 */
int db_msg_get(int msgid, int *from_id, char *content, int csz) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT from_id,content FROM messages WHERE id=%d", msgid);
    LOCK();
    int rc = -1;
    if (!mysql_query(g_conn, sql)) {
        MYSQL_RES *r = mysql_store_result(g_conn);
        MYSQL_ROW row;
        if (r && (row = mysql_fetch_row(r))) {
            if (from_id) *from_id = atoi(row[0]);
            if (content) { strncpy(content, row[1] ? row[1] : "", csz - 1); content[csz - 1] = 0; }
            rc = 0;
        }
        if (r) mysql_free_result(r);
    }
    UNLOCK();
    return rc;
}

/* 给离线用户 uid 记一条待收消息: 只存 message_id 引用(正文已在 messages 表),
 * 等对方上线时 db_offline_take 再取出投递。0/-1。 */
int db_offline_put(int uid, int mid) {
    char sql[128];
    snprintf(sql, sizeof(sql),
        "INSERT INTO offline_msg(user_id,message_id) VALUES(%d,%d)", uid, mid);
    LOCK(); int rc = mysql_query(g_conn, sql); UNLOCK();
    return rc == 0 ? 0 : -1;
}

/* 公共取行逻辑: 执行一条"六列固定顺序"的 SELECT
 *   (from_id, target_id, msg_type, content, sent_at, from_nick)
 * 把每行填进 OfflineRow 数组, 返回行数。离线消息 / 私聊历史 / 群聊历史
 * 三处共用这个函数, 只是传入的 SQL 不同。 */
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

/* 取出并清空 uid 的离线消息: 先按消息 id 升序(时间顺序)取回最多 max 条,
 * 再 DELETE 掉该用户的全部离线记录(取走即消费)。返回取到的条数。 */
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

/* 拉取 a 与 b 之间的私聊历史(双向, 谁发给谁都算), 按时间升序, 最多 max 条。 */
int db_history_priv(int a, int b, OfflineRow *rows, int max) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT m.from_id,m.target_id,m.msg_type,m.content,m.sent_at,u.nickname "
        "FROM messages m JOIN users u ON u.id=m.from_id "
        "WHERE m.msg_type=0 AND ((m.from_id=%d AND m.target_id=%d) OR (m.from_id=%d AND m.target_id=%d)) "
        "ORDER BY m.id ASC LIMIT %d", a, b, b, a, max);
    return fetch_rows(sql, rows, max);
}

/* 拉取群 gid 的群聊历史, 按时间升序, 最多 max 条。 */
int db_history_group(int gid, OfflineRow *rows, int max) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "SELECT m.from_id,m.target_id,m.msg_type,m.content,m.sent_at,u.nickname "
        "FROM messages m JOIN users u ON u.id=m.from_id "
        "WHERE m.msg_type=1 AND m.target_id=%d ORDER BY m.id ASC LIMIT %d", gid, max);
    return fetch_rows(sql, rows, max);
}

/* ===== 好友申请 ===== */
/* 提交一条好友申请(from_id 想加 to_id, hello 是招呼语)。
 * 幂等设计: 若已存在同向且未处理(status=0)的申请, 就复用它并更新 hello,
 * 避免重复点"加好友"产生一堆待处理记录。返回 reqid, 失败 -1。 */
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

/* 按 reqid 取出申请的双方 id(审批时用来校验 to_id 就是当前用户)。0/-1。 */
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

/* 标记申请处理结果: status 1=同意 2=拒绝(0 表示仍待处理)。0/-1。 */
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

/* ===== 入群申请 =====
 * 结构与好友申请完全对称, 只是对象换成 (群, 用户), 审批人是群主。 */
/* 提交入群申请, 已有未处理的同向申请则复用并更新 hello。返回 reqid / -1。 */
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

/* 按 reqid 取出申请对应的群号与申请人 id。0/-1。 */
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

/* 标记入群申请结果: status 1=同意 2=拒绝。0/-1。 */
int db_greq_set(int reqid, int status) {
    char sql[128];
    snprintf(sql, sizeof(sql),
        "UPDATE group_join_requests SET status=%d WHERE id=%d", status, reqid);
    LOCK(); int rc = mysql_query(g_conn, sql); UNLOCK();
    return rc == 0 ? 0 : -1;
}

/* 列出 owner_id 名下所有群的待处理入群申请(供群主审批面板)。
 * 每行 "reqid\tgid\tgname\tfrom_nick\ttime\thello\n"。三表 join:
 * 申请表 -> 群(筛 owner_id) -> 申请人(取昵称)。 */
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

/* 取群主 uid。群不存在返回 -1。用于权限判断(改公告/审批入群仅群主可为)。 */
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

/* 取群名填进 out。0/-1。 */
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

/* 取群成员人数(搜索结果里展示用)。 */
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

/* 按群名模糊搜索群。每行 "gid\tname\towner_nick\tmember_count\n"。
 * LEFT JOIN group_members + GROUP BY 顺带统计成员数。最多 50 条。 */
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

/* 给好友设置备注名(只改 uid 这一侧的 friends.remark, 备注是"我看对方"的私有信息)。 */
int db_friend_set_remark(int uid, int fid, const char *remark) {
    char en[128]; esc(remark, en, sizeof(en));
    char sql[256];
    snprintf(sql, sizeof(sql),
        "UPDATE friends SET remark='%s' WHERE user_id=%d AND friend_id=%d", en, uid, fid);
    LOCK(); int rc = mysql_query(g_conn, sql) ? -1 : 0; UNLOCK();
    return rc;
}

/* uid 是否是群 gid 的成员。1/0。发群消息/群文件/邀请前的权限校验。 */
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

/* 直接把 uid 拉进群(成员邀请路径, 无需群主审批)。INSERT IGNORE 幂等。0/-1。 */
int db_group_add_member(int gid, int uid) {
    char sql[160];
    snprintf(sql, sizeof(sql),
        "INSERT IGNORE INTO group_members(group_id,user_id) VALUES(%d,%d)", gid, uid);
    LOCK(); int rc = mysql_query(g_conn, sql) ? -1 : 0; UNLOCK();
    return rc;
}

/* 退群: 删掉 uid 在 gid 里的成员行(群主退群的限制在 handler 层拦截)。0/-1。 */
int db_group_leave(int gid, int uid) {
    char sql[160];
    snprintf(sql, sizeof(sql),
        "DELETE FROM group_members WHERE group_id=%d AND user_id=%d", gid, uid);
    LOCK(); int rc = mysql_query(g_conn, sql) ? -1 : 0; UNLOCK();
    return rc;
}

/* 取群公告文本填进 out(NULL 视作空串)。0/-1。 */
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

/* 设置群公告(权限校验在 handler 层)。0/-1。 */
int db_group_set_notice(int gid, const char *notice) {
    char en[1100]; esc(notice, en, sizeof(en));
    char sql[1300];
    snprintf(sql, sizeof(sql), "UPDATE chat_groups SET notice='%s' WHERE id=%d", en, gid);
    LOCK(); int rc = mysql_query(g_conn, sql) ? -1 : 0; UNLOCK();
    return rc;
}

/* 设置生日。传入空串则置 NULL(清空)。只取前 10 个字符即 "YYYY-MM-DD",
 * 且字段是纯数字和横杠、格式固定, 故这里未 escape。 */
int db_set_birthday(int uid, const char *birth) {
    char sql[160];
    if (birth && birth[0])
        snprintf(sql, sizeof(sql), "UPDATE users SET birthday='%.10s' WHERE id=%d", birth, uid);
    else
        snprintf(sql, sizeof(sql), "UPDATE users SET birthday=NULL WHERE id=%d", uid);
    LOCK(); int rc = mysql_query(g_conn, sql) ? -1 : 0; UNLOCK();
    return rc;
}

/* 修改昵称。0/-1。 */
int db_set_nick(int uid, const char *nick) {
    char en[128]; esc(nick, en, sizeof(en));
    char sql[256];
    snprintf(sql, sizeof(sql), "UPDATE users SET nickname='%s' WHERE id=%d", en, uid);
    LOCK(); int rc = mysql_query(g_conn, sql) ? -1 : 0; UNLOCK();
    return rc;
}

/* 设置头像色号(=客户端选择的动物形象下标). 让好友端 colorAnimal(color) 能还原
 * 出与本人一致的动物头像; 否则注册时 avatar_color 只是昵称 hash, 两端不一致. */
int db_set_avatar_color(int uid, int color) {
    if (color < 0) return -1;
    char sql[128];
    snprintf(sql, sizeof(sql), "UPDATE users SET avatar_color=%d WHERE id=%d", color, uid);
    LOCK(); int rc = mysql_query(g_conn, sql) ? -1 : 0; UNLOCK();
    return rc;
}

/* 一次取回资料三件套: 昵称 / 头像色 / 生日。任一 out 指针可为 NULL 表示不取。
 * 生日用 DATE_FORMAT 统一成 "YYYY-MM-DD" 字符串(SQL 里 %% 是给 snprintf 转义,
 * 下到 MySQL 是单 %)。0/-1。 */
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
