#ifndef CHAT_DB_H
#define CHAT_DB_H

#include "../common/protocol.h"

/* 全局连接初始化/关闭. 多线程共用一把互斥锁串行化. */
int  db_init(const char *host, const char *user, const char *pass, const char *dbname);
void db_close(void);

/* 用户.
 * 账号 = 100000 + id, 客户端看到的字符串 "100001" 直接代表 id=1. */
#define ACCOUNT_BASE  100000
int  db_register(const char *nickname, const char *pass);          /* 返回 id 或 -错误码 */
int  db_login_by_id(int user_id, const char *pass);                /* 返回 user_id 或 -错误码 */
int  db_user_id_by_account(const char *account);                   /* 字符串→id, 错误返回 -1 */
int  db_user_id_by_nick(const char *nick);                         /* 取昵称第一个匹配的 id */
int  db_get_nick (int user_id, char *out, int outsz);              /* 0/-1 */
int  db_get_avatar_color(int user_id);                             /* 0..9 */
int  db_set_online(int user_id, int online);

/* 好友/黑名单 */
int  db_friend_add  (int uid, int fid);
int  db_friend_del  (int uid, int fid);
int  db_black_set   (int uid, int fid, int black);                 /* 0=普通,1=拉黑 */
int  db_is_friend   (int uid, int fid);                            /* 1/0 */
int  db_is_black    (int uid, int fid);                            /* 1/0 */
/* 把好友列表填进 body:
 * "account\tnick\tcolor\tonline\tblack\tremark\n..." . is_online 查询在线状态 */
int  db_friend_list (int uid, char *out, int outsz,
                     int (*is_online)(int user_id));
/* 设置好友备注 */
int  db_friend_set_remark(int uid, int fid, const char *remark);

/* 群组 */
int  db_group_create(int owner_id, const char *name);              /* 返回 gid */
int  db_group_join  (int gid, int uid);
int  db_group_list_for_user(int uid, char *out, int outsz);
int  db_group_members(int gid, int *ids, int max);                 /* 返回个数 */
int  db_group_is_member(int gid, int uid);                         /* 1/0 */
int  db_group_add_member(int gid, int uid);                        /* INSERT IGNORE, 0/-1 */
int  db_group_notice_get(int gid, char *out, int outsz);           /* 取群公告, 0/-1 */
int  db_group_set_notice(int gid, const char *notice);             /* 设群公告, 0/-1 */

/* ===== 个人资料 ===== */
int  db_set_birthday(int uid, const char *birth);                  /* birth 空=置 NULL */
int  db_set_nick    (int uid, const char *nick);
/* 取资料: nick/birth 填字符串, *color 填头像色; 0/-1 */
int  db_profile_get (int uid, char *nick, int nsz, char *birth, int bsz, int *color);

/* 消息 */
int  db_save_msg   (int from, int target, int type, const char *content); /* 返回 msg_id */
int  db_offline_put(int user_id, int msg_id);
/* 取出离线消息, 填到 out 数组(每个含 from/target/type/body/time), 返回个数; 之后会删除. */
typedef struct {
    int  from_id;
    int  target_id;
    int  msg_type;        /* 0 私 1 群 */
    char from_name[MAX_NAME_LEN];
    char content[MAX_BODY_LEN];
    char sent_at[32];
} OfflineRow;
int  db_offline_take(int user_id, OfflineRow *rows, int max);

/* 历史: rows 同上(无 sent_at 影响), 按时间升序 */
int  db_history_priv (int uid_a, int uid_b, OfflineRow *rows, int max);
int  db_history_group(int gid,             OfflineRow *rows, int max);

/* ===== 好友申请 ===== */
/* 插入待处理申请, 返回 reqid; 若已有同向 pending 直接复用. */
int  db_freq_put   (int from_id, int to_id, const char *hello);
/* 取出申请的元信息(填到 *from_id, *to_id) */
int  db_freq_info  (int reqid, int *from_id, int *to_id);
/* 标记 status; 1=同意, 2=拒绝 */
int  db_freq_set   (int reqid, int status);
/* 拉取某用户的待处理申请: 每行 "reqid\tfrom_name\ttime\thello" */
int  db_freq_list  (int to_id, char *out, int outsz);

/* ===== 入群申请 ===== */
int  db_greq_put   (int gid, int user_id, const char *hello);
int  db_greq_info  (int reqid, int *gid, int *user_id);
int  db_greq_set   (int reqid, int status);
/* 当前用户作为群主, 列出所有自己拥有的群的待处理入群申请:
 * "reqid\tgid\tgname\tfrom_name\ttime\thello" */
int  db_greq_list_for_owner(int owner_id, char *out, int outsz);

/* 群主 id */
int  db_group_owner(int gid);
int  db_group_name (int gid, char *out, int outsz);
/* 群成员个数 */
int  db_group_member_count(int gid);

/* ===== 搜索 ===== */
/* 模糊搜用户/群, 返回行数. 用户行: "name\tonline\n"; 群行: "gid\tname\towner\tcnt\n" */
int  db_user_search (const char *q, char *out, int outsz,
                     int (*is_online)(int));
int  db_group_search(const char *q, char *out, int outsz);

/* 全文搜索. 搜 user_id 参与过的所有消息(私聊收发 + 加入的群聊),
 * 命中 LIKE %q% 的, 按时间倒序, 最多 50 条. 输出行格式:
 *   "msg_id\tsent_at\tfrom_nick\tkind\tpeer\tsnippet" */
int  db_msg_search  (int user_id, const char *q, char *out, int outsz);

#endif
