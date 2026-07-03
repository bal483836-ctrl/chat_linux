/* =========================================================
 *  handler.c - 每客户端连接的业务处理线程
 *
 *  server.c 每 accept 一个连接就起一个线程跑 client_thread(),
 *  该线程循环 recv_msg() 收一个定长 Message, 按 m.type 分派到对应
 *  分支处理, 再用 send_msg()/online_push() 回应或转发。
 *
 *  一个线程 = 一个会话(Session), 会话状态(登录用户、文件上传缓冲)都在
 *  栈上的 Session 结构里, 天然线程私有; 跨会话共享的只有在线表(online.c)
 *  和数据库(db.c), 它们各自内部加锁。
 *
 *  消息流转的三条通路(理解本文件的关键):
 *    - 在线直达: online_push() 直接往对方 socket 写;
 *    - 离线兜底: 对方不在线时把 msg_id 记进 offline_msg 表, 等其上线补发;
 *    - 历史回放: 所有消息都先 db_save_msg 落库, 客户端可随时拉历史。
 * ========================================================= */
#include "handler.h"
#include "online.h"
#include "db.h"
#include "../common/net_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>

/* 服务器端单文件上限(比客户端 6MB 略宽, 防御性) */
#define MAX_FILE_BYTES (8 * 1024 * 1024)
#define SRV_FILE_CHUNK 2048
/* 文件消息在 messages.content 里的标记: 首字节 0x01 + "FILE\t..." */
#define FILE_TAG "\001FILE\t"

/* 当前线程持有的会话状态 */
typedef struct {
    int  fd;
    int  uid;                       /* 已登录用户 id, 未登录为 -1 */
    char account[MAX_NAME_LEN];     /* "100001" */
    char nick[MAX_NAME_LEN];        /* 昵称 */
    /* 文件上传重组缓冲(一次上传一个文件) */
    unsigned char *up_buf;
    size_t         up_cap, up_len;
    uint32_t       up_size;         /* 客户端声明的总字节 */
    int            up_is_group;
    int            up_target;       /* 私聊=对方 uid, 群聊=gid */
    char           up_name[128];
    char           up_mime[64];
} Session;

/* 给客户端回一条统一应答 MSG_RESPONSE: status 是结果码(RS_*), text 是描述文字。
 * 客户端据此弹提示/判断成功失败。 */
static void resp(int fd, int status, const char *text) {
    Message m;
    memset(&m, 0, sizeof(m));
    m.type   = MSG_RESPONSE;
    m.status = status;
    if (text) {
        strncpy(m.body, text, MAX_BODY_LEN - 1);
        m.body_len = strlen(m.body);
    }
    fill_timestamp(m.timestamp, sizeof(m.timestamp));
    send_msg(fd, &m);
}

/* 把 body 里的 "用户\n口令" 拆成两段填进 u / p。返回 0 成功 / -1 格式错。
 *   strchr 找换行符定位分隔点; nl-body 即用户名长度(指针相减得字节数);
 *   用户名长度要在 (0, MAX_NAME_LEN) 内; 口令取换行之后的部分, 限长拷贝。 */
static int split_userpass(const char *body, char *u, char *p) {
    const char *nl = strchr(body, '\n');       /* 找 '\n' 分隔 */
    if (!nl) return -1;                          /* 没有换行 = 格式非法 */
    int ulen = nl - body;                        /* 换行前的长度就是用户名长度 */
    if (ulen <= 0 || ulen >= MAX_NAME_LEN) return -1;
    memcpy(u, body, ulen); u[ulen] = 0;          /* 拷贝用户名并补 '\0' */
    strncpy(p, nl + 1, MAX_PASS_LEN - 1);        /* 换行之后是口令 */
    return 0;
}

/* 供 db_friend_list 等做实时在线判定的回调: 内存在线表里查得到 fd 即在线。 */
static int is_user_online(int uid) { return online_get_fd_by_id(uid) >= 0; }

/* 头像 key 是否为群头像("g"+纯数字, 如 "g5")。返回 1=是群头像 0=否。
 * 逐字符校验 'g' 之后必须全是数字, 这既区分群/个人头像, 也顺带挡住形如
 * "../.." 的路径穿越(非法字符直接判否), 后续才敢拿它拼文件名。 */
static int avatar_group_key(const char *key) {
    if (!key || key[0] != 'g' || !key[1]) return 0;   /* 必须以 'g' 开头且后面非空 */
    for (const char *p = key + 1; *p; ++p) if (*p < '0' || *p > '9') return 0;
    return 1;
}

/* 广播我的上/下线状态给"加了我的好友"。
 * 做法: 拉我的好友列表(每行 "账号\t...\n"), 逐行取出第一列账号, 换算成 uid,
 * 若对方在线就 push 一条 NOTIFY_ONLINE/OFFLINE, 让对方的界面实时刷新在线灯。
 * online 参数: 1=上线 0=下线, 决定发哪种通知类型。 */
static void notify_friends(int uid, const char *account, const char *nick, int online) {
    char list[8192]; list[0] = 0;
    db_friend_list(uid, list, sizeof(list), is_user_online);   /* 取好友列表文本 */
    /* 组一条通知消息模板: 发送者就是"我" */
    Message n;
    memset(&n, 0, sizeof(n));
    n.type = online ? MSG_NOTIFY_ONLINE : MSG_NOTIFY_OFFLINE;
    strncpy(n.from_name, account, MAX_NAME_LEN - 1);
    strncpy(n.from_nick, nick,    MAX_NAME_LEN - 1);
    fill_timestamp(n.timestamp, sizeof(n.timestamp));
    /* strtok 按 '\n' 切成一行行(会就地改写 list, 所以 list 是可写副本) */
    char *line = strtok(list, "\n");
    while (line) {
        char *tab = strchr(line, '\t');   /* 每行首列是账号, 到第一个 '\t' 为止 */
        if (tab) {
            *tab = 0;            /* 把 '\t' 截成 '\0', line 就成了纯账号字符串 */
            int fid = db_user_id_by_account(line);
            if (fid > 0) online_push(fid, &n);   /* 对方在线才推(不在线无所谓) */
        }
        line = strtok(NULL, "\n");   /* 下一行; strtok 靠内部静态状态续切 */
    }
}

/* 处理一条私聊消息, 是"在线直达 + 离线兜底 + 落库存档"三合一的典型流程。 */
static void do_private(Session *s, Message *m) {
    if (s->uid < 0) { resp(s->fd, RS_AUTH_FAIL, "not login"); return; }   /* 未登录拒绝 */
    int to = db_user_id_by_account(m->to_name);                          /* 收方账号->uid */
    if (to < 0) { resp(s->fd, RS_USER_NOT_FOUND, m->to_name); return; }   /* 查无此人 */
    if (db_is_black(to, s->uid)) {                                       /* 我被对方拉黑 */
        resp(s->fd, RS_IN_BLACKLIST, "对方将你拉黑, 无法发送");
        return;
    }
    /* 无论对方在不在线, 先落库拿到 msg_id(历史/离线都靠它) */
    int mid = db_save_msg(s->uid, to, 0, m->body);
    /* 以收到的消息为模板改造成"服务器转发版": 补上真实发送者账号/昵称/时间戳,
     * 覆盖客户端可能乱填的字段, 防伪造。 */
    Message out = *m;
    out.type = MSG_PRIVATE_CHAT;
    strncpy(out.from_name, s->account, MAX_NAME_LEN - 1);
    strncpy(out.from_nick, s->nick,    MAX_NAME_LEN - 1);
    fill_timestamp(out.timestamp, sizeof(out.timestamp));
    /* 尝试在线直推; push 失败(对方不在线)且已落库, 就记入离线队列等其上线补发 */
    if (online_push(to, &out) < 0 && mid > 0) db_offline_put(to, mid);
    resp(s->fd, RS_OK, "sent");   /* 给发送方回执 */
}

/* 把最新群成员列表推给群里的每个在线成员(退群/邀请后调用, 让大家实时看到变化) */
static void push_group_members(int gid) {
    int ids[1024];
    int n = db_group_members(gid, ids, 1024);
    if (n <= 0) return;
    Message o; memset(&o, 0, sizeof(o));
    o.type = MSG_GROUP_MEMBERS; o.group_id = gid;
    int used = 0;
    for (int i = 0; i < n; ++i) {
        char nick[MAX_NAME_LEN] = {0};
        db_get_nick(ids[i], nick, sizeof(nick));
        int color  = db_get_avatar_color(ids[i]);
        int online = (online_get_fd_by_id(ids[i]) >= 0) ? 1 : 0;
        int k = snprintf(o.body + used, MAX_BODY_LEN - used,
                         "%d\t%s\t%d\t%d\n", ids[i] + ACCOUNT_BASE, nick, color, online);
        if (k <= 0 || k >= MAX_BODY_LEN - used) break;
        used += k;
    }
    o.body_len = used;
    fill_timestamp(o.timestamp, sizeof(o.timestamp));
    online_broadcast(ids, n, &o, -1);
}

/* 处理一条群聊消息: 与私聊类似, 只是收方从"一个人"变成"群里每个成员"。 */
static void do_group(Session *s, Message *m) {
    if (s->uid < 0) { resp(s->fd, RS_AUTH_FAIL, "not login"); return; }
    int members[1024];
    int n = db_group_members(m->group_id, members, 1024);            /* 取全体成员 */
    if (n <= 0) { resp(s->fd, RS_GROUP_NOT_FOUND, "no such group"); return; }
    int mid = db_save_msg(s->uid, m->group_id, 1, m->body);          /* 落库(type=1 群聊) */
    Message out = *m;                                                /* 同样重填发送者信息 */
    out.type = MSG_GROUP_CHAT;
    strncpy(out.from_name, s->account, MAX_NAME_LEN - 1);
    strncpy(out.from_nick, s->nick,    MAX_NAME_LEN - 1);
    fill_timestamp(out.timestamp, sizeof(out.timestamp));
    /* 遍历成员逐个投递: 跳过自己(发送者不用收自己的消息);
     * 在线则直推, 不在线则给这一个成员单独入离线队列。 */
    for (int i = 0; i < n; ++i) {
        if (members[i] == s->uid) continue;
        if (online_push(members[i], &out) < 0 && mid > 0)
            db_offline_put(members[i], mid);
    }
    resp(s->fd, RS_OK, "sent");
}

/* 把一条已落库的消息(msgid)派发给接收者: 在线直接推, 离线入离线队列。
 * 文件消息与文本消息共用此通路, 保证离线可达 + 历史可拉。
 * is_group=1: target=gid, 推给除自己外全部成员; 否则 target=对方 uid。 */
static void dispatch_saved(Session *s, int is_group, int target, int msgid, const char *content) {
    /* 先拼好一条完整的转发消息(内容由调用方给, 通常是文件消息标记串) */
    Message out;
    memset(&out, 0, sizeof(out));
    out.type = is_group ? MSG_GROUP_CHAT : MSG_PRIVATE_CHAT;
    out.group_id = is_group ? target : 0;                       /* 群聊带群号 */
    strncpy(out.from_name, s->account, MAX_NAME_LEN - 1);
    strncpy(out.from_nick, s->nick,    MAX_NAME_LEN - 1);
    if (!is_group)                                              /* 私聊补收方账号 */
        snprintf(out.to_name, MAX_NAME_LEN, "%d", target + ACCOUNT_BASE);
    fill_timestamp(out.timestamp, sizeof(out.timestamp));
    strncpy(out.body, content, MAX_BODY_LEN - 1);
    out.body_len = strlen(out.body);
    /* 投递: 群聊遍历成员(跳过自己), 私聊只发对方; 均是"在线直推, 否则入离线" */
    if (is_group) {
        int members[1024];
        int n = db_group_members(target, members, 1024);
        for (int i = 0; i < n; ++i) {
            if (members[i] == s->uid) continue;
            if (online_push(members[i], &out) < 0 && msgid > 0)
                db_offline_put(members[i], msgid);
        }
    } else {
        if (online_push(target, &out) < 0 && msgid > 0)
            db_offline_put(target, msgid);
    }
    /* 文件消息额外回推给发送者自己一份: 让客户端给"本地乐观显示的那条"补上 fileid,
     * 从而历史重拉时能按 fileid 去重(不影响文本消息, 文本不走此函数)。 */
    if (memcmp(out.body, FILE_TAG, 6) == 0) online_push(s->uid, &out);
}

/* 把一批数据库取出的行(离线消息或历史记录)逐条转成 Message 发给客户端。
 * 复用于三处: 登录补发离线、拉私聊历史、拉群聊历史。
 * 注意 OfflineRow 里存的是内部 id, 这里都要 +ACCOUNT_BASE 还原成对外账号;
 * from_name 字段在 OfflineRow 里其实存的是发送者昵称(见 fetch_rows)。 */
static void push_history(int fd, OfflineRow *rows, int n) {
    for (int i = 0; i < n; ++i) {
        Message m;
        memset(&m, 0, sizeof(m));
        /* msg_type 0/1 映射到私聊/群聊消息类型 */
        m.type     = rows[i].msg_type == 0 ? MSG_PRIVATE_CHAT : MSG_GROUP_CHAT;
        m.group_id = rows[i].msg_type == 1 ? rows[i].target_id : 0;    /* 群聊带群号 */
        snprintf(m.from_name, MAX_NAME_LEN, "%d", rows[i].from_id + ACCOUNT_BASE);
        /* 私聊历史补上 to_name=接收方账号, 客户端才能正确归到对应会话 */
        if (rows[i].msg_type == 0)
            snprintf(m.to_name, MAX_NAME_LEN, "%d", rows[i].target_id + ACCOUNT_BASE);
        strncpy(m.from_nick, rows[i].from_name, MAX_NAME_LEN - 1);   /* 发送者昵称 */
        strncpy(m.timestamp, rows[i].sent_at,   sizeof(m.timestamp) - 1);
        strncpy(m.body,      rows[i].content,   MAX_BODY_LEN - 1);
        m.body_len = strlen(m.body);
        send_msg(fd, &m);   /* 逐条同步发出 */
    }
}

/* 登录成功后的"开场推送": 客户端刚进主界面, 服务器主动把它需要的初始数据
 * 一股脑推过去, 于是界面一显示就是满的。顺序: 好友列表 -> 群列表 -> 待处理
 * 好友申请 -> 待处理入群申请(仅群主) -> 补发离线消息 -> 广播我已上线。
 * 每一项都是"建一个对应 type 的 Message, 用 db_* 把内容填进 body, send_msg 发出"。 */
static void on_login_success(Session *s) {
    /* 上线状态写库(持久化冗余, 实时在线仍以内存表为准) */
    db_set_online(s->uid, 1);
    /* 推送好友列表给自己 */
    Message lst;
    memset(&lst, 0, sizeof(lst));
    lst.type = MSG_FRIEND_LIST;
    int used = db_friend_list(s->uid, lst.body, MAX_BODY_LEN, is_user_online);
    lst.body_len = used;
    send_msg(s->fd, &lst);
    /* 推送群组列表 */
    Message gl;
    memset(&gl, 0, sizeof(gl));
    gl.type = MSG_GROUP_LIST;
    gl.body_len = db_group_list_for_user(s->uid, gl.body, MAX_BODY_LEN);
    send_msg(s->fd, &gl);
    /* 推送待处理好友/入群申请 */
    Message fr; memset(&fr, 0, sizeof(fr));
    fr.type = MSG_FRIEND_REQ_LIST;
    fr.body_len = db_freq_list(s->uid, fr.body, MAX_BODY_LEN);
    send_msg(s->fd, &fr);
    Message gr; memset(&gr, 0, sizeof(gr));
    gr.type = MSG_GROUP_JOIN_REQ_LIST;
    gr.body_len = db_greq_list_for_owner(s->uid, gr.body, MAX_BODY_LEN);
    send_msg(s->fd, &gr);
    /* 取离线消息 */
    OfflineRow rows[64];
    int n = db_offline_take(s->uid, rows, 64);
    if (n > 0) push_history(s->fd, rows, n);
    /* 通知好友本人上线 */
    notify_friends(s->uid, s->account, s->nick, 1);
}

/* 连接处理线程入口(server.c 里 pthread_create 的起点)。
 * arg 是堆上的 int*(socket fd), 取出后立即 free。detach 自己, 线程结束即回收。
 * 主体是"收一条 -> switch 分派 -> 处理"的循环, recv_msg 返回非 0(对端关闭
 * 或出错)即跳出, 走 out: 做登出清理。 */
void *client_thread(void *arg) {
    int fd = *(int *)arg;
    free(arg);
    pthread_detach(pthread_self());

    Session sess = { .fd = fd, .uid = -1 };   /* uid=-1 表示尚未登录 */
    Message m;

    while (recv_msg(fd, &m) == 0) {
        switch (m.type) {
        case MSG_REGISTER: {
            /* 注册: body 是 "昵称\n口令[\n邮箱]"(邮箱可选)。这里手工按两个 '\n'
             * 切三段。相比 split_userpass, 多了可选的第三段邮箱, 故单独解析。 */
            char nick[MAX_NAME_LEN] = {0}, p[MAX_PASS_LEN] = {0}, email[64] = {0};
            const char *nl1 = strchr(m.body, '\n');            /* 第一个换行: 昵称/口令分界 */
            if (!nl1) { resp(fd, RS_FAIL, "bad format"); break; }
            int nlen = nl1 - m.body; if (nlen <= 0 || nlen >= MAX_NAME_LEN) { resp(fd, RS_FAIL, "bad format"); break; }
            memcpy(nick, m.body, nlen); nick[nlen] = 0;         /* 第一段=昵称 */
            const char *l2 = nl1 + 1;                           /* 从口令段开头继续 */
            const char *nl2 = strchr(l2, '\n');                 /* 第二个换行: 口令/邮箱分界 */
            if (nl2) {                                          /* 有第三段=带邮箱 */
                int plen = nl2 - l2; if (plen >= MAX_PASS_LEN) plen = MAX_PASS_LEN - 1;
                memcpy(p, l2, plen); p[plen] = 0;               /* 第二段=口令 */
                strncpy(email, nl2 + 1, sizeof(email) - 1);     /* 第三段=邮箱 */
            } else {                                            /* 没有第三段=无邮箱 */
                strncpy(p, l2, MAX_PASS_LEN - 1);
            }
            /* 邮箱查重(有邮箱才查): 命中直接回"已注册", 避免 DB 唯一键报错 */
            if (email[0] && db_email_exists(email)) { resp(fd, RS_USER_EXIST, "该邮箱已注册"); break; }
            int id = db_register(nick, p, email);
            if (id < 0) { resp(fd, RS_FAIL, "注册失败(邮箱可能已被使用)"); break; }
            /* === 自动加 mock 好友 ===
             * 把 sql/init.sql 里预置的"小助手 / 新手指南"两位补成新用户的
             * 初始好友, 让登录后好友列表不至于空荡荡, 也方便答辩演示头像/
             * 在线状态/会话等基础功能. 没拿到 id (例如老库里没建这俩) 就跳过. */
            static const char *DEMOS[] = {"小助手", "新手指南"};
            for (int i = 0; i < 2; ++i) {
                int demo_id = db_user_id_by_nick(DEMOS[i]);
                if (demo_id > 0 && demo_id != id) db_friend_add(id, demo_id);
            }
            /* 返回分配的账号 (字符串) */
            char acc[16]; snprintf(acc, sizeof(acc), "%d", id + ACCOUNT_BASE);
            resp(fd, RS_OK, acc);
            break;
        }
        case MSG_LOGIN: {
            /* 登录: 带 '@' 当邮箱登录, 否则当账号(数字)登录。
             * 成功后: 记录会话(uid/account/nick) -> 加入在线表 -> 回一条 body
             * 为 "账号\n昵称" 的应答 -> on_login_success 推好友/群/申请/离线消息。 */
            /* body = "account或email\npassword" */
            char first[MAX_NAME_LEN] = {0}, p[MAX_PASS_LEN] = {0};
            if (split_userpass(m.body, first, p) < 0) { resp(fd, RS_FAIL, "bad format"); break; }
            int id;
            if (strchr(first, '@')) {          /* 含 '@' -> 当邮箱登录 */
                id = db_login_by_email(first, p);
            } else {                            /* 否则当账号(数字)登录 */
                int uid = atoi(first) - ACCOUNT_BASE;             /* 账号还原 uid */
                if (uid <= 0) { resp(fd, RS_AUTH_FAIL, "账号格式错误"); break; }
                id = db_login_by_id(uid, p);
            }
            if (id < 0) { resp(fd, RS_AUTH_FAIL, "账号或密码错误"); break; }  /* 认证失败 */
            /* 认证通过: 把登录态写进本线程的会话结构 sess */
            char acc[MAX_NAME_LEN]; snprintf(acc, sizeof(acc), "%d", id + ACCOUNT_BASE);
            sess.uid = id;
            strncpy(sess.account, acc, MAX_NAME_LEN - 1);
            db_get_nick(id, sess.nick, sizeof(sess.nick));       /* 缓存昵称, 转发消息时用 */
            online_add(id, acc, fd);                             /* 登记到在线表(同账号会挤掉旧连接) */
            /* 应答 body = "<account>\n<nickname>" 客户端解析 */
            Message rr; memset(&rr, 0, sizeof(rr));
            rr.type = MSG_RESPONSE; rr.status = RS_OK;
            snprintf(rr.body, sizeof(rr.body), "%s\n%s", acc, sess.nick);
            rr.body_len = strlen(rr.body);
            fill_timestamp(rr.timestamp, sizeof(rr.timestamp));
            send_msg(fd, &rr);
            on_login_success(&sess);
            break;
        }
        case MSG_LOGOUT:
            goto out;                       /* 主动登出: 跳到清理逻辑 */

        case MSG_PRIVATE_CHAT: do_private(&sess, &m); break;
        case MSG_GROUP_CHAT:   do_group  (&sess, &m); break;

        /* ===== 好友 / 黑名单增删 =====
         * 都是"账号字符串 -> uid -> 调 db -> 回应"的固定套路。这里的 FRIEND_ADD
         * 是兼容用的直接互加; 正式流程走下面的 FRIEND_REQ 申请-审批。 */
        case MSG_FRIEND_ADD: {
            int fid = db_user_id_by_account(m.to_name);
            if (fid < 0) { resp(fd, RS_USER_NOT_FOUND, m.to_name); break; }
            db_friend_add(sess.uid, fid);
            resp(fd, RS_OK, "friend added");
            break;
        }
        case MSG_FRIEND_DEL: {
            int fid = db_user_id_by_account(m.to_name);
            if (fid < 0) { resp(fd, RS_USER_NOT_FOUND, m.to_name); break; }
            db_friend_del(sess.uid, fid);
            resp(fd, RS_OK, "friend deleted");
            break;
        }
        case MSG_BLACK_ADD: {
            int fid = db_user_id_by_account(m.to_name);
            if (fid < 0) { resp(fd, RS_USER_NOT_FOUND, m.to_name); break; }
            db_black_set(sess.uid, fid, 1);
            resp(fd, RS_OK, "blacklisted");
            break;
        }
        case MSG_BLACK_DEL: {
            int fid = db_user_id_by_account(m.to_name);
            if (fid < 0) { resp(fd, RS_USER_NOT_FOUND, m.to_name); break; }
            db_black_set(sess.uid, fid, 0);
            resp(fd, RS_OK, "unblacklisted");
            break;
        }
        case MSG_FRIEND_LIST: {
            Message o; memset(&o, 0, sizeof(o));
            o.type = MSG_FRIEND_LIST;
            o.body_len = db_friend_list(sess.uid, o.body, MAX_BODY_LEN, is_user_online);
            send_msg(fd, &o);
            break;
        }
        /* 建群: body=群名, 成功后把新群号(字符串)放进应答 body 回给客户端 */
        case MSG_GROUP_CREATE: {
            int gid = db_group_create(sess.uid, m.body);
            if (gid < 0) resp(fd, RS_FAIL, "create group failed");
            else { char t[32]; snprintf(t, sizeof(t), "%d", gid); resp(fd, RS_OK, t); }
            break;
        }
        /* 兼容路径: 直接加群(不审批); 正式流程见下面 GROUP_JOIN_REQ */
        case MSG_GROUP_JOIN:
            db_group_join(m.group_id, sess.uid);
            resp(fd, RS_OK, "joined");
            break;

        case MSG_GROUP_LIST: {
            Message o; memset(&o, 0, sizeof(o));
            o.type = MSG_GROUP_LIST;
            o.body_len = db_group_list_for_user(sess.uid, o.body, MAX_BODY_LEN);
            send_msg(fd, &o);
            break;
        }

        /* 群成员列表: "account\tnick\tcolor\tonline\n" */
        case MSG_GROUP_MEMBERS: {
            if (sess.uid < 0) { resp(fd, RS_AUTH_FAIL, "not login"); break; }
            int ids[1024];
            int n = db_group_members(m.group_id, ids, 1024);
            Message o; memset(&o, 0, sizeof(o));
            o.type = MSG_GROUP_MEMBERS; o.group_id = m.group_id;
            int used = 0;
            for (int i = 0; i < n; ++i) {
                char nick[MAX_NAME_LEN] = {0};
                db_get_nick(ids[i], nick, sizeof(nick));
                int color  = db_get_avatar_color(ids[i]);
                int online = (online_get_fd_by_id(ids[i]) >= 0) ? 1 : 0;
                int k = snprintf(o.body + used, MAX_BODY_LEN - used,
                                 "%d\t%s\t%d\t%d\n", ids[i] + ACCOUNT_BASE, nick, color, online);
                if (k <= 0 || k >= MAX_BODY_LEN - used) break;
                used += k;
            }
            o.body_len = used;
            send_msg(fd, &o);
            break;
        }

        /* ===== Web 原型新增功能 ===== */

        /* 设置好友备注 */
        case MSG_FRIEND_REMARK: {
            if (sess.uid < 0) { resp(fd, RS_AUTH_FAIL, "not login"); break; }
            int fid = db_user_id_by_account(m.to_name);
            if (fid < 0) { resp(fd, RS_USER_NOT_FOUND, m.to_name); break; }
            db_friend_set_remark(sess.uid, fid, m.body);
            resp(fd, RS_OK, "remark set");
            break;
        }

        /* 成员邀请好友入群: group_id=群号, body 每行一个账号 */
        case MSG_GROUP_INVITE: {
            if (sess.uid < 0) { resp(fd, RS_AUTH_FAIL, "not login"); break; }
            if (!db_group_is_member(m.group_id, sess.uid)) { resp(fd, RS_FAIL, "你不是该群成员"); break; }
            int added = 0;
            char dup[MAX_BODY_LEN]; strncpy(dup, m.body, sizeof(dup) - 1); dup[sizeof(dup) - 1] = 0;
            char *save = NULL, *line = strtok_r(dup, "\n", &save);
            while (line) {
                while (*line == ' ' || *line == '\r') ++line;
                int tid = db_user_id_by_account(line);
                if (tid > 0 && db_group_add_member(m.group_id, tid) == 0) {
                    ++added;
                    /* 把更新后的群列表推给被邀请者(在线时) */
                    Message gl; memset(&gl, 0, sizeof(gl));
                    gl.type = MSG_GROUP_LIST;
                    gl.body_len = db_group_list_for_user(tid, gl.body, MAX_BODY_LEN);
                    online_push(tid, &gl);
                }
                line = strtok_r(NULL, "\n", &save);
            }
            char t[48]; snprintf(t, sizeof(t), "已邀请 %d 人", added);
            resp(fd, RS_OK, t);
            break;
        }

        /* 群公告: body 非空=群主设置, 否则查询; 回 MSG_GROUP_NOTICE */
        case MSG_GROUP_NOTICE: {
            if (sess.uid < 0) { resp(fd, RS_AUTH_FAIL, "not login"); break; }
            if (m.body_len > 0) {
                if (db_group_owner(m.group_id) != sess.uid) { resp(fd, RS_FAIL, "仅群主可修改公告"); break; }
                db_group_set_notice(m.group_id, m.body);
            }
            Message o; memset(&o, 0, sizeof(o));
            o.type = MSG_GROUP_NOTICE; o.group_id = m.group_id;
            db_group_notice_get(m.group_id, o.body, MAX_BODY_LEN);
            o.body_len = strlen(o.body);
            fill_timestamp(o.timestamp, sizeof(o.timestamp));
            send_msg(fd, &o);
            break;
        }

        /* 查询个人资料: to_name=账号(空=自己) */
        case MSG_PROFILE_GET: {
            if (sess.uid < 0) { resp(fd, RS_AUTH_FAIL, "not login"); break; }
            int tid = (m.to_name[0]) ? db_user_id_by_account(m.to_name) : sess.uid;
            if (tid < 0) { resp(fd, RS_USER_NOT_FOUND, m.to_name); break; }
            char nick[MAX_NAME_LEN] = {0}, birth[16] = {0}; int color = 0;
            db_profile_get(tid, nick, sizeof(nick), birth, sizeof(birth), &color);
            Message o; memset(&o, 0, sizeof(o));
            o.type = MSG_PROFILE_DATA;
            snprintf(o.from_name, MAX_NAME_LEN, "%d", tid + ACCOUNT_BASE);
            int online = (online_get_fd_by_id(tid) >= 0) ? 1 : 0;
            o.body_len = snprintf(o.body, MAX_BODY_LEN, "%s\t%s\t%d\t%d", nick, birth, color, online);
            send_msg(fd, &o);
            break;
        }

        /* 更新自己资料: body="昵称\n出生日期" */
        case MSG_PROFILE_SET: {
            if (sess.uid < 0) { resp(fd, RS_AUTH_FAIL, "not login"); break; }
            char nick[MAX_NAME_LEN] = {0}, birth[16] = {0};
            const char *nl = strchr(m.body, '\n');
            if (nl) {
                int nlen = nl - m.body; if (nlen >= MAX_NAME_LEN) nlen = MAX_NAME_LEN - 1;
                memcpy(nick, m.body, nlen); nick[nlen] = 0;
                strncpy(birth, nl + 1, sizeof(birth) - 1);
            } else {
                strncpy(nick, m.body, sizeof(nick) - 1);
            }
            if (nick[0]) { db_set_nick(sess.uid, nick); strncpy(sess.nick, nick, MAX_NAME_LEN - 1); }
            db_set_birthday(sess.uid, birth);
            /* status = 头像动物下标+1 (0=不修改). 持久化选择的动物形象,
             * 好友端 colorAnimal(avatar_color) 才能还原出与本人一致的头像. */
            if (m.status > 0) db_set_avatar_color(sess.uid, (int)m.status - 1);
            resp(fd, RS_OK, "profile updated");
            break;
        }

        case MSG_HISTORY_PRIV: {
            int other = db_user_id_by_account(m.to_name);
            if (other < 0) { resp(fd, RS_USER_NOT_FOUND, m.to_name); break; }
            OfflineRow rows[64];
            int n = db_history_priv(sess.uid, other, rows, 64);
            push_history(fd, rows, n);
            break;
        }
        case MSG_HISTORY_GROUP: {
            OfflineRow rows[64];
            int n = db_history_group(m.group_id, rows, 64);
            push_history(fd, rows, n);
            break;
        }

        /* ===== 文件/图片上传, 一个三阶段状态机: BEGIN -> CHUNK... -> END =====
         * 状态存在 Session 的 up_* 字段里。BEGIN 记录元信息并分配缓冲, 每个 CHUNK
         * 往缓冲追加分片, END 时把缓冲整体落盘 + 落库成一条"文件消息", 然后走和文本
         * 一样的分发通路(在线推/离线入队), 于是文件也支持离线送达和拉历史。 */

        /* 阶段一: 开始上传。status=文件总字节数, body="文件名\tMIME"。 */
        case MSG_FILE_BEGIN: {
            if (sess.uid < 0) { resp(fd, RS_AUTH_FAIL, "not login"); break; }
            /* 若上一次上传没正常收尾, 先清掉残留缓冲, 从干净状态开始 */
            free(sess.up_buf); sess.up_buf = NULL; sess.up_len = 0; sess.up_cap = 0;
            sess.up_size = m.status;                          /* 客户端声明的总大小 */
            if (sess.up_size == 0 || sess.up_size > MAX_FILE_BYTES) { resp(fd, RS_FAIL, "文件为空或过大"); break; }
            /* 解析 body 里的 "文件名\tMIME": 用可写副本 b, 把 '\t' 切成 '\0' 分两段 */
            char b[MAX_BODY_LEN]; int bl = m.body_len < MAX_BODY_LEN ? m.body_len : MAX_BODY_LEN - 1;
            memcpy(b, m.body, bl); b[bl] = 0;
            char *tab = strchr(b, '\t');
            if (tab) { *tab = 0; strncpy(sess.up_mime, tab + 1, sizeof(sess.up_mime) - 1); sess.up_mime[sizeof(sess.up_mime)-1]=0; }
            else sess.up_mime[0] = 0;                         /* 没给 MIME 就留空 */
            strncpy(sess.up_name, b, sizeof(sess.up_name) - 1); sess.up_name[sizeof(sess.up_name)-1]=0;
            /* 确定收方: group_id>0 是发到群(要先校验群成员身份), 否则发给某人 */
            if (m.group_id > 0) {
                if (!db_group_is_member(m.group_id, sess.uid)) { resp(fd, RS_FAIL, "你不是该群成员"); break; }
                sess.up_is_group = 1; sess.up_target = m.group_id;
            } else {
                int to = db_user_id_by_account(m.to_name);
                if (to < 0) { resp(fd, RS_USER_NOT_FOUND, m.to_name); break; }
                sess.up_is_group = 0; sess.up_target = to;
            }
            /* 按声明大小一次性预分配重组缓冲(malloc(0) 未定义, 故至少要 1 字节) */
            sess.up_cap = sess.up_size;
            sess.up_buf = (unsigned char *)malloc(sess.up_cap ? sess.up_cap : 1);
            if (!sess.up_buf) { resp(fd, RS_FAIL, "服务器内存不足"); }
            break;
        }
        /* 阶段二: 收到一个分片, 追加到缓冲末尾。可能来很多个。 */
        case MSG_FILE_CHUNK: {
            if (sess.uid < 0 || !sess.up_buf) break;          /* 没在上传态就忽略 */
            size_t need = sess.up_len + m.body_len;           /* 追加后所需总长 */
            if (need > sess.up_cap) {                          /* 声明大小偏小的容错扩容 */
                if (need > MAX_FILE_BYTES) { free(sess.up_buf); sess.up_buf = NULL; break; }  /* 超硬上限, 放弃 */
                unsigned char *nb = (unsigned char *)realloc(sess.up_buf, need);
                if (!nb) { free(sess.up_buf); sess.up_buf = NULL; break; }   /* 扩容失败, 放弃 */
                sess.up_buf = nb; sess.up_cap = need;
            }
            memcpy(sess.up_buf + sess.up_len, m.body, m.body_len);  /* 拷到当前写指针处 */
            sess.up_len += m.body_len;                        /* 前移写指针 */
            break;
        }
        /* 阶段三: 上传结束, 落盘 + 落库 + 分发。 */
        case MSG_FILE_END: {
            if (sess.uid < 0 || !sess.up_buf) break;
            /* 巧妙之处: 先存一条占位消息拿到自增 msgid, 直接把它当作 fileid(文件名),
             * 这样 fileid 天然唯一, 无需另建 id 生成器。 */
            int msgid = db_save_msg(sess.uid, sess.up_target, sess.up_is_group ? 1 : 0, FILE_TAG "pending");
            if (msgid > 0) {
                mkdir("data", 0755); mkdir("data/files", 0755);   /* 确保目录存在(已存在则无害) */
                char path[128]; snprintf(path, sizeof(path), "data/files/%d", msgid);
                FILE *fp = fopen(path, "wb");
                if (fp) { fwrite(sess.up_buf, 1, sess.up_len, fp); fclose(fp); }  /* 整块写盘 */
                /* 回填正文为带完整元信息的文件标记: \001FILE\t<fileid>\t<名>\t<MIME>\t<大小> */
                char content[256];
                snprintf(content, sizeof(content), FILE_TAG "%d\t%s\t%s\t%u",
                         msgid, sess.up_name, sess.up_mime, sess.up_size);
                db_update_msg_content(msgid, content);
                /* 走通用分发: 收方在线收到文件消息, 不在线则入离线队列 */
                dispatch_saved(&sess, sess.up_is_group, sess.up_target, msgid, content);
            }
            free(sess.up_buf); sess.up_buf = NULL; sess.up_len = sess.up_cap = 0;   /* 收尾清理 */
            /* 不回 RESPONSE(避免污染客户端应答队列); fileid 通过上面回推的文件消息补给发送方 */
            break;
        }
        /* 下载: 客户端给 fileid(status), 服务器读盘, 用同样的 BEGIN/CHUNK/END 三段发回。
         * 下行时 group_id 字段被复用来携带 fileid, 客户端据此把分片拼回对应的那条消息。 */
        case MSG_FILE_GET: {
            if (sess.uid < 0) { resp(fd, RS_AUTH_FAIL, "not login"); break; }
            int fileid = (int)m.status;
            int from_id = -1; char content[256] = {0};
            if (db_msg_get(fileid, &from_id, content, sizeof(content)) < 0) break;   /* 查不到该消息 */
            if (memcmp(content, FILE_TAG, 6) != 0) break;   /* 前 6 字节不是文件标记, 说明不是文件消息 */
            /* 解析 content = \001FILE\t<fileid>\t<name>\t<mime>\t<size>
             * 用 strchr 逐个找 '\t', 就地改成 '\0' 把各段切开, 指针指向每段开头。 */
            char *p = content + 6;                          /* 跳过 6 字节的 "\001FILE\t" 标记 */
            char *f1 = strchr(p, '\t'); if (!f1) break;     /* f1 指向 fileid 段之后的 '\t' */
            char *name = f1 + 1;                            /* 文件名从这里开始 */
            char *f2 = strchr(name, '\t'); if (!f2) break; *f2 = 0;   /* 截断文件名 */
            char *mime = f2 + 1;
            char *f3 = strchr(mime, '\t'); if (!f3) break; *f3 = 0;   /* 截断 MIME */
            uint32_t size = (uint32_t)atol(f3 + 1);         /* 最后一段是大小 */
            char path[128]; snprintf(path, sizeof(path), "data/files/%d", fileid);
            FILE *fp = fopen(path, "rb");
            if (!fp) { resp(fd, RS_FAIL, "文件不存在"); break; }   /* 库里有记录但盘上文件丢了 */
            /* 带上原发送者账号/昵称, 客户端展示"谁发的文件" */
            char fromacc[MAX_NAME_LEN]; snprintf(fromacc, sizeof(fromacc), "%d", from_id + ACCOUNT_BASE);
            char fromnick[MAX_NAME_LEN] = {0}; db_get_nick(from_id, fromnick, sizeof(fromnick));
            /* (1) 先发 FILE_BEGIN: 带上文件名/MIME/总大小 */
            Message o; memset(&o, 0, sizeof(o));
            o.type = MSG_FILE_BEGIN; o.group_id = (uint32_t)fileid; o.status = size;
            strncpy(o.from_name, fromacc, MAX_NAME_LEN - 1);
            strncpy(o.from_nick, fromnick, MAX_NAME_LEN - 1);
            o.body_len = snprintf(o.body, MAX_BODY_LEN, "%s\t%s", name, mime);
            send_msg(fd, &o);
            /* (2) 循环读盘, 每读一块发一个 FILE_CHUNK */
            unsigned char buf[SRV_FILE_CHUNK]; size_t r;
            while ((r = fread(buf, 1, sizeof(buf), fp)) > 0) {
                Message c; memset(&c, 0, sizeof(c));
                c.type = MSG_FILE_CHUNK; c.group_id = (uint32_t)fileid;
                memcpy(c.body, buf, r); c.body_len = (uint32_t)r;
                send_msg(fd, &c);
            }
            fclose(fp);
            /* (3) 最后发 FILE_END 收尾, 客户端据此知道文件传完了 */
            Message e; memset(&e, 0, sizeof(e));
            e.type = MSG_FILE_END; e.group_id = (uint32_t)fileid;
            send_msg(fd, &e);
            break;
        }

        /* ===== 好友申请流程 ===== */
        case MSG_FRIEND_REQ: {
            int to = db_user_id_by_account(m.to_name);
            if (to < 0) { resp(fd, RS_USER_NOT_FOUND, m.to_name); break; }
            if (to == sess.uid) { resp(fd, RS_FAIL, "不能加自己为好友"); break; }
            if (db_is_friend(sess.uid, to)) {
                resp(fd, RS_FAIL, "已经是好友"); break;
            }
            int reqid = db_freq_put(sess.uid, to, m.body);
            if (reqid < 0) { resp(fd, RS_FAIL, "申请失败"); break; }
            /* 通知目标 */
            Message n; memset(&n, 0, sizeof(n));
            n.type   = MSG_FRIEND_REQ_NOTIFY;
            n.status = reqid;
            strncpy(n.from_name, sess.account, MAX_NAME_LEN - 1);
            strncpy(n.from_nick, sess.nick,    MAX_NAME_LEN - 1);
            strncpy(n.body, m.body, MAX_BODY_LEN - 1);
            n.body_len = strlen(n.body);
            fill_timestamp(n.timestamp, sizeof(n.timestamp));
            online_push(to, &n);
            resp(fd, RS_OK, "已发送好友申请");
            break;
        }
        case MSG_FRIEND_REQ_REPLY: {
            int reqid = (int)m.status;
            int accept = m.group_id == 1;
            int from_id = -1, to_id = -1;
            if (db_freq_info(reqid, &from_id, &to_id) < 0 || to_id != sess.uid) {
                resp(fd, RS_FAIL, "无效申请"); break;
            }
            db_freq_set(reqid, accept ? 1 : 2);
            if (accept) {
                db_friend_add(from_id, to_id);
                /* 双方都刷一下好友列表 */
                Message fl; memset(&fl, 0, sizeof(fl));
                fl.type = MSG_FRIEND_LIST;
                fl.body_len = db_friend_list(sess.uid, fl.body, MAX_BODY_LEN, is_user_online);
                send_msg(fd, &fl);
                /* 通知申请方 */
                if (online_get_fd_by_id(from_id) >= 0) {
                    Message fl2; memset(&fl2, 0, sizeof(fl2));
                    fl2.type = MSG_FRIEND_LIST;
                    fl2.body_len = db_friend_list(from_id, fl2.body, MAX_BODY_LEN, is_user_online);
                    online_push(from_id, &fl2);
                    /* 也推一条 RESPONSE 提示 */
                    Message rr; memset(&rr, 0, sizeof(rr));
                    rr.type = MSG_RESPONSE; rr.status = RS_OK;
                    snprintf(rr.body, sizeof(rr.body),
                        "%s 同意了你的好友申请", sess.nick);
                    rr.body_len = strlen(rr.body);
                    online_push(from_id, &rr);
                }
            }
            resp(fd, RS_OK, accept ? "已同意" : "已拒绝");
            break;
        }
        case MSG_FRIEND_REQ_LIST: {
            Message o; memset(&o, 0, sizeof(o));
            o.type = MSG_FRIEND_REQ_LIST;
            o.body_len = db_freq_list(sess.uid, o.body, MAX_BODY_LEN);
            send_msg(fd, &o);
            break;
        }

        /* ===== 入群申请流程 ===== */
        case MSG_GROUP_JOIN_REQ: {
            int gid = m.group_id;
            int owner = db_group_owner(gid);
            if (owner < 0) { resp(fd, RS_GROUP_NOT_FOUND, "群不存在"); break; }
            if (owner == sess.uid) { resp(fd, RS_FAIL, "你是群主, 无需申请"); break; }
            int reqid = db_greq_put(gid, sess.uid, m.body);
            if (reqid < 0) { resp(fd, RS_FAIL, "申请失败"); break; }
            /* 通知群主 */
            Message n; memset(&n, 0, sizeof(n));
            n.type     = MSG_GROUP_JOIN_NOTIFY;
            n.status   = reqid;
            n.group_id = gid;
            strncpy(n.from_name, sess.account, MAX_NAME_LEN - 1);
            strncpy(n.from_nick, sess.nick,    MAX_NAME_LEN - 1);
            db_group_name(gid, n.to_name, MAX_NAME_LEN);
            strncpy(n.body, m.body, MAX_BODY_LEN - 1);
            n.body_len = strlen(n.body);
            fill_timestamp(n.timestamp, sizeof(n.timestamp));
            online_push(owner, &n);
            resp(fd, RS_OK, "已发送入群申请, 等待群主审批");
            break;
        }
        case MSG_GROUP_JOIN_REPLY: {
            int reqid = (int)m.status;
            int accept = m.group_id == 1;
            int gid = -1, applier = -1;
            if (db_greq_info(reqid, &gid, &applier) < 0) {
                resp(fd, RS_FAIL, "无效申请"); break;
            }
            if (db_group_owner(gid) != sess.uid) {
                resp(fd, RS_FAIL, "只有群主可审批"); break;
            }
            db_greq_set(reqid, accept ? 1 : 2);
            if (accept) {
                db_group_join(gid, applier);
                /* 通知申请方刷新群列表 */
                if (online_get_fd_by_id(applier) >= 0) {
                    Message gl; memset(&gl, 0, sizeof(gl));
                    gl.type = MSG_GROUP_LIST;
                    gl.body_len = db_group_list_for_user(applier, gl.body, MAX_BODY_LEN);
                    online_push(applier, &gl);
                    Message rr; memset(&rr, 0, sizeof(rr));
                    rr.type = MSG_RESPONSE; rr.status = RS_OK;
                    char gname[64] = {0}; db_group_name(gid, gname, sizeof(gname));
                    snprintf(rr.body, sizeof(rr.body),
                        "群主同意你加入群【%s】", gname);
                    rr.body_len = strlen(rr.body);
                    online_push(applier, &rr);
                }
            }
            resp(fd, RS_OK, accept ? "已同意入群" : "已拒绝");
            break;
        }
        case MSG_GROUP_JOIN_REQ_LIST: {
            Message o; memset(&o, 0, sizeof(o));
            o.type = MSG_GROUP_JOIN_REQ_LIST;
            o.body_len = db_greq_list_for_owner(sess.uid, o.body, MAX_BODY_LEN);
            send_msg(fd, &o);
            break;
        }

        /* 退出群聊: 群主不能直接退出(先转让/解散), 其余成员直接移除membership */
        case MSG_GROUP_LEAVE: {
            if (sess.uid < 0) { resp(fd, RS_AUTH_FAIL, "not login"); break; }
            int gid = m.group_id;
            if (!db_group_is_member(gid, sess.uid)) { resp(fd, RS_FAIL, "你不在该群"); break; }
            if (db_group_owner(gid) == sess.uid) { resp(fd, RS_FAIL, "群主不能退出群聊"); break; }
            db_group_leave(gid, sess.uid);
            push_group_members(gid);   /* 让其余成员实时看到你已退出 */
            resp(fd, RS_OK, "已退出群聊");
            break;
        }

        /* ===== 头像上传/拉取 =====
         * 上传: 客户端把 PNG 字节塞进 body, status 写真实字节数 (因为 body
         * 是定长缓冲, 真实数据后面可能有 0). 我们落到 data/avatars/<uid>.png.
         * 拉取: 客户端给账号, 我们 open file 读字节, 装进 MSG_AVATAR_DATA
         * 回去. 文件不存在时 status=0, 让客户端回退到首字母圆形头像. */
        case MSG_AVATAR_UPLOAD: {
            if (sess.uid < 0) { resp(fd, RS_AUTH_FAIL, "未登录"); break; }
            uint32_t sz = m.status;
            if (sz == 0 || sz > MAX_BODY_LEN) { resp(fd, RS_FAIL, "头像数据非法"); break; }
            mkdir("data", 0755);
            mkdir("data/avatars", 0755);
            char path[128]; int is_group = 0, gid = 0;
            if (avatar_group_key(m.to_name)) {
                gid = atoi(m.to_name + 1);
                if (!db_group_is_member(gid, sess.uid)) { resp(fd, RS_FAIL, "你不是该群成员"); break; }
                snprintf(path, sizeof(path), "data/avatars/g%d.png", gid); is_group = 1;
            } else {
                snprintf(path, sizeof(path), "data/avatars/%d.png", sess.uid);
            }
            FILE *fp = fopen(path, "wb");
            if (!fp) { resp(fd, RS_FAIL, "保存失败"); break; }
            fwrite(m.body, 1, sz, fp);
            fclose(fp);
            /* 主动把新头像推给相关在线用户, 解决对方"负缓存"(之前拉过一次为空
             * 就不再拉)导致头像更新后对方看不到的问题. */
            Message av; memset(&av, 0, sizeof(av));
            av.type = MSG_AVATAR_DATA; av.status = sz; av.body_len = sz;
            memcpy(av.body, m.body, sz);
            if (is_group) {
                snprintf(av.from_name, MAX_NAME_LEN, "g%d", gid);
                int ids[1024]; int n = db_group_members(gid, ids, 1024);
                for (int i = 0; i < n; ++i) if (ids[i] != sess.uid) online_push(ids[i], &av);
            } else {
                strncpy(av.from_name, sess.account, MAX_NAME_LEN - 1);
                char list[8192]; list[0] = 0;
                db_friend_list(sess.uid, list, sizeof(list), is_user_online);
                char *line = strtok(list, "\n");
                while (line) {
                    char *tab = strchr(line, '\t'); if (tab) *tab = 0;
                    int fid = db_user_id_by_account(line);
                    if (fid > 0) online_push(fid, &av);
                    line = strtok(NULL, "\n");
                }
            }
            resp(fd, RS_OK, "头像已更新");
            break;
        }
        case MSG_AVATAR_GET: {
            Message o; memset(&o, 0, sizeof(o));
            o.type = MSG_AVATAR_DATA;
            strncpy(o.from_name, m.to_name, MAX_NAME_LEN - 1);
            char path[128]; int ok = 0;
            if (avatar_group_key(m.to_name)) {
                snprintf(path, sizeof(path), "data/avatars/g%d.png", atoi(m.to_name + 1)); ok = 1;
            } else {
                int uid = db_user_id_by_account(m.to_name);
                if (uid > 0) { snprintf(path, sizeof(path), "data/avatars/%d.png", uid); ok = 1; }
            }
            if (ok) {
                FILE *fp = fopen(path, "rb");
                if (fp) {
                    size_t n = fread(o.body, 1, MAX_BODY_LEN, fp);
                    fclose(fp);
                    o.body_len = (uint32_t)n;
                    o.status   = (uint32_t)n;
                }
            }
            send_msg(fd, &o);
            break;
        }

        /* ===== 搜索 ===== */
        case MSG_USER_SEARCH: {
            Message o; memset(&o, 0, sizeof(o));
            o.type = MSG_USER_SEARCH;
            o.body_len = db_user_search(m.body, o.body, MAX_BODY_LEN, is_user_online);
            send_msg(fd, &o);
            break;
        }
        case MSG_GROUP_SEARCH: {
            Message o; memset(&o, 0, sizeof(o));
            o.type = MSG_GROUP_SEARCH;
            o.body_len = db_group_search(m.body, o.body, MAX_BODY_LEN);
            send_msg(fd, &o);
            break;
        }
        /* 消息检索: 必须登录, 只搜本人参与过的对话 */
        case MSG_MSG_SEARCH: {
            if (sess.uid < 0) { resp(fd, RS_AUTH_FAIL, "未登录"); break; }
            Message o; memset(&o, 0, sizeof(o));
            o.type = MSG_MSG_SEARCH;
            o.body_len = db_msg_search(sess.uid, m.body, o.body, MAX_BODY_LEN);
            send_msg(fd, &o);
            break;
        }

        default:
            resp(fd, RS_FAIL, "unknown msg");
        }
    }

out:
    /* 连接结束清理: 释放未完成的文件上传缓冲; 若已登录则写下线状态并广播给
     * 好友, 再从在线表摘除本 fd, 最后关闭 socket。 */
    free(sess.up_buf);
    if (sess.uid > 0) {
        db_set_online(sess.uid, 0);
        notify_friends(sess.uid, sess.account, sess.nick, 0);
    }
    online_remove_by_fd(fd);
    close(fd);
    return NULL;
}
