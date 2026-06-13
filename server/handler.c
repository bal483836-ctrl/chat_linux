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

/* 当前线程持有的会话状态 */
typedef struct {
    int  fd;
    int  uid;                       /* 已登录用户 id, 未登录为 -1 */
    char name[MAX_NAME_LEN];
} Session;

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

/* 拆分 "user\npass" */
static int split_userpass(const char *body, char *u, char *p) {
    const char *nl = strchr(body, '\n');
    if (!nl) return -1;
    int ulen = nl - body;
    if (ulen <= 0 || ulen >= MAX_NAME_LEN) return -1;
    memcpy(u, body, ulen); u[ulen] = 0;
    strncpy(p, nl + 1, MAX_PASS_LEN - 1);
    return 0;
}

static int is_user_online(int uid) { return online_get_fd_by_id(uid) >= 0; }

/* 通知好友: 上下线广播 */
static void notify_friends(int uid, const char *uname, int online) {
    /* 用于查询此人的所有"反向好友": 即把我加为好友的人 */
    char list[8192]; list[0] = 0;
    db_friend_list(uid, list, sizeof(list), is_user_online);
    /* 遍历: 给每个在线好友推一条 */
    Message n;
    memset(&n, 0, sizeof(n));
    n.type = online ? MSG_NOTIFY_ONLINE : MSG_NOTIFY_OFFLINE;
    strncpy(n.from_name, uname, MAX_NAME_LEN - 1);
    fill_timestamp(n.timestamp, sizeof(n.timestamp));
    char *line = strtok(list, "\n");
    while (line) {
        char *tab = strchr(line, '\t');
        if (tab) {
            *tab = 0;
            int fid = db_user_id(line);
            if (fid > 0) online_push(fid, &n);
        }
        line = strtok(NULL, "\n");
    }
}

/* 处理私聊 */
static void do_private(Session *s, Message *m) {
    if (s->uid < 0) { resp(s->fd, R_AUTH_FAIL, "not login"); return; }
    int to = db_user_id(m->to_name);
    if (to < 0) { resp(s->fd, R_USER_NOT_FOUND, m->to_name); return; }
    if (db_is_black(to, s->uid)) {
        resp(s->fd, R_IN_BLACKLIST, "you are in target's blacklist");
        return;
    }
    /* 持久化 */
    int mid = db_save_msg(s->uid, to, 0, m->body);
    /* 转发 */
    Message out = *m;
    out.type = MSG_PRIVATE_CHAT;
    strncpy(out.from_name, s->name, MAX_NAME_LEN - 1);
    fill_timestamp(out.timestamp, sizeof(out.timestamp));
    if (online_push(to, &out) < 0 && mid > 0) {
        db_offline_put(to, mid);
    }
    resp(s->fd, R_OK, "sent");
}

/* 处理群聊 */
static void do_group(Session *s, Message *m) {
    if (s->uid < 0) { resp(s->fd, R_AUTH_FAIL, "not login"); return; }
    int members[1024];
    int n = db_group_members(m->group_id, members, 1024);
    if (n <= 0) { resp(s->fd, R_GROUP_NOT_FOUND, "no such group"); return; }
    int mid = db_save_msg(s->uid, m->group_id, 1, m->body);
    Message out = *m;
    out.type = MSG_GROUP_CHAT;
    strncpy(out.from_name, s->name, MAX_NAME_LEN - 1);
    fill_timestamp(out.timestamp, sizeof(out.timestamp));
    /* 在线广播 + 离线入库 */
    for (int i = 0; i < n; ++i) {
        if (members[i] == s->uid) continue;
        if (online_push(members[i], &out) < 0 && mid > 0)
            db_offline_put(members[i], mid);
    }
    resp(s->fd, R_OK, "sent");
}

/* 把若干条历史/离线消息推送给指定 fd */
static void push_history(int fd, OfflineRow *rows, int n) {
    for (int i = 0; i < n; ++i) {
        Message m;
        memset(&m, 0, sizeof(m));
        m.type     = rows[i].msg_type == 0 ? MSG_PRIVATE_CHAT : MSG_GROUP_CHAT;
        m.group_id = rows[i].msg_type == 1 ? rows[i].target_id : 0;
        strncpy(m.from_name, rows[i].from_name, MAX_NAME_LEN - 1);
        strncpy(m.timestamp, rows[i].sent_at,   sizeof(m.timestamp) - 1);
        strncpy(m.body,      rows[i].content,   MAX_BODY_LEN - 1);
        m.body_len = strlen(m.body);
        send_msg(fd, &m);
    }
}

/* 登录成功后的初始化: 推送好友列表 + 离线消息 */
static void on_login_success(Session *s) {
    /* 上线状态写库 */
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
    /* 取离线消息 */
    OfflineRow rows[64];
    int n = db_offline_take(s->uid, rows, 64);
    if (n > 0) push_history(s->fd, rows, n);
    /* 通知好友本人上线 */
    notify_friends(s->uid, s->name, 1);
}

void *client_thread(void *arg) {
    int fd = *(int *)arg;
    free(arg);
    pthread_detach(pthread_self());

    Session sess = { .fd = fd, .uid = -1, .name = {0} };
    Message m;

    while (recv_msg(fd, &m) == 0) {
        switch (m.type) {
        case MSG_REGISTER: {
            char u[MAX_NAME_LEN] = {0}, p[MAX_PASS_LEN] = {0};
            if (split_userpass(m.body, u, p) < 0) { resp(fd, R_FAIL, "bad format"); break; }
            int id = db_register(u, p);
            if (id < 0) resp(fd, -id, "register failed");
            else        resp(fd, R_OK, "registered");
            break;
        }
        case MSG_LOGIN: {
            char u[MAX_NAME_LEN] = {0}, p[MAX_PASS_LEN] = {0};
            if (split_userpass(m.body, u, p) < 0) { resp(fd, R_FAIL, "bad format"); break; }
            int id = db_login(u, p);
            if (id < 0) { resp(fd, R_AUTH_FAIL, "login failed"); break; }
            sess.uid = id;
            strncpy(sess.name, u, MAX_NAME_LEN - 1);
            online_add(id, u, fd);
            resp(fd, R_OK, u);
            on_login_success(&sess);
            break;
        }
        case MSG_LOGOUT:
            goto out;

        case MSG_PRIVATE_CHAT: do_private(&sess, &m); break;
        case MSG_GROUP_CHAT:   do_group  (&sess, &m); break;

        case MSG_FRIEND_ADD: {
            int fid = db_user_id(m.to_name);
            if (fid < 0) { resp(fd, R_USER_NOT_FOUND, m.to_name); break; }
            db_friend_add(sess.uid, fid);
            resp(fd, R_OK, "friend added");
            break;
        }
        case MSG_FRIEND_DEL: {
            int fid = db_user_id(m.to_name);
            if (fid < 0) { resp(fd, R_USER_NOT_FOUND, m.to_name); break; }
            db_friend_del(sess.uid, fid);
            resp(fd, R_OK, "friend deleted");
            break;
        }
        case MSG_BLACK_ADD: {
            int fid = db_user_id(m.to_name);
            if (fid < 0) { resp(fd, R_USER_NOT_FOUND, m.to_name); break; }
            db_black_set(sess.uid, fid, 1);
            resp(fd, R_OK, "blacklisted");
            break;
        }
        case MSG_BLACK_DEL: {
            int fid = db_user_id(m.to_name);
            if (fid < 0) { resp(fd, R_USER_NOT_FOUND, m.to_name); break; }
            db_black_set(sess.uid, fid, 0);
            resp(fd, R_OK, "unblacklisted");
            break;
        }
        case MSG_FRIEND_LIST: {
            Message o; memset(&o, 0, sizeof(o));
            o.type = MSG_FRIEND_LIST;
            o.body_len = db_friend_list(sess.uid, o.body, MAX_BODY_LEN, is_user_online);
            send_msg(fd, &o);
            break;
        }
        case MSG_GROUP_CREATE: {
            int gid = db_group_create(sess.uid, m.body);
            if (gid < 0) resp(fd, R_FAIL, "create group failed");
            else { char t[32]; snprintf(t, sizeof(t), "%d", gid); resp(fd, R_OK, t); }
            break;
        }
        case MSG_GROUP_JOIN:
            db_group_join(m.group_id, sess.uid);
            resp(fd, R_OK, "joined");
            break;

        case MSG_GROUP_LIST: {
            Message o; memset(&o, 0, sizeof(o));
            o.type = MSG_GROUP_LIST;
            o.body_len = db_group_list_for_user(sess.uid, o.body, MAX_BODY_LEN);
            send_msg(fd, &o);
            break;
        }
        case MSG_HISTORY_PRIV: {
            int other = db_user_id(m.to_name);
            if (other < 0) { resp(fd, R_USER_NOT_FOUND, m.to_name); break; }
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

        /* 文件传输: 服务器作为中继, 透明转发 */
        case MSG_FILE_BEGIN:
        case MSG_FILE_CHUNK:
        case MSG_FILE_END: {
            int to = db_user_id(m.to_name);
            if (to < 0) { resp(fd, R_USER_NOT_FOUND, m.to_name); break; }
            Message f = m;
            strncpy(f.from_name, sess.name, MAX_NAME_LEN - 1);
            online_push(to, &f);
            break;
        }

        default:
            resp(fd, R_FAIL, "unknown msg");
        }
    }

out:
    if (sess.uid > 0) {
        db_set_online(sess.uid, 0);
        notify_friends(sess.uid, sess.name, 0);
    }
    online_remove_by_fd(fd);
    close(fd);
    return NULL;
}
