#include "client.h"
#include "../common/net_io.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

ClientCtx CTX;
static pthread_mutex_t send_mu = PTHREAD_MUTEX_INITIALIZER;

int net_connect(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) <= 0) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(fd); return -1; }
    CTX.sockfd = fd;
    return 0;
}

void net_close(void) {
    if (CTX.sockfd >= 0) { close(CTX.sockfd); CTX.sockfd = -1; }
}

int net_send(const Message *m) {
    pthread_mutex_lock(&send_mu);
    int r = send_msg(CTX.sockfd, m);
    pthread_mutex_unlock(&send_mu);
    return r;
}

int net_send_text(int type, const char *to, int gid, const char *text) {
    Message m;
    memset(&m, 0, sizeof(m));
    m.type     = type;
    m.group_id = gid;
    if (to)   strncpy(m.to_name,   to,   MAX_NAME_LEN - 1);
    if (text) {
        strncpy(m.body, text, MAX_BODY_LEN - 1);
        m.body_len = strlen(m.body);
    }
    fill_timestamp(m.timestamp, sizeof(m.timestamp));
    return net_send(&m);
}

/* ---- 把接收到的消息派发回 GTK 主线程 ---- */
typedef struct { Message m; } DispatchPack;

static gboolean dispatch_in_main(gpointer data) {
    DispatchPack *p = (DispatchPack *)data;
    Message *m = &p->m;
    switch (m->type) {
    case MSG_RESPONSE:
        /* 简单实现: 直接把 server 的提示打到聊天框, 帮助调试 */
        ui_append_chat("[server]", m->timestamp, m->body);
        break;
    case MSG_PRIVATE_CHAT:
    case MSG_GROUP_CHAT: {
        char who[96];
        /* from_nick 是昵称 (服务器填), from_name 是账号 */
        const char *nick = m->from_nick[0] ? m->from_nick : m->from_name;
        if (m->type == MSG_GROUP_CHAT)
            snprintf(who, sizeof(who), "%s@群%u", nick, m->group_id);
        else
            snprintf(who, sizeof(who), "%s", nick);
        ui_append_chat(who, m->timestamp, m->body);
        break;
    }
    case MSG_FRIEND_LIST:
        ui_refresh_friends(m->body);
        break;
    case MSG_GROUP_LIST:
        ui_refresh_groups(m->body);
        break;
    case MSG_NOTIFY_ONLINE: {
        char s[128];
        snprintf(s, sizeof(s), "好友 %s 上线", m->from_nick[0] ? m->from_nick : m->from_name);
        ui_append_chat("[通知]", m->timestamp, s);
        net_send_text(MSG_FRIEND_LIST, NULL, 0, NULL);
        break;
    }
    case MSG_NOTIFY_OFFLINE: {
        char s[128];
        snprintf(s, sizeof(s), "好友 %s 下线", m->from_nick[0] ? m->from_nick : m->from_name);
        ui_append_chat("[通知]", m->timestamp, s);
        net_send_text(MSG_FRIEND_LIST, NULL, 0, NULL);
        break;
    }
    case MSG_FILE_BEGIN: {
        char s[128];
        snprintf(s, sizeof(s), "收到来自 %s 的文件 %s (%u 字节), 已保存到 recv/",
                 m->from_name, m->body, m->status);
        ui_append_chat("[文件]", m->timestamp, s);
        break;
    }

    /* ===== 申请流程相关 ===== */
    case MSG_FRIEND_REQ_NOTIFY: {
        /* from_name=申请人账号, from_nick=申请人昵称 */
        int color = 0;
        for (const char *q = m->from_nick; *q; ++q) color = (color * 131 + (unsigned char)*q) & 0xFFFF;
        color %= 10;
        ui_add_request(0, (int)m->status, m->from_name, m->from_nick, color,
                       NULL, m->body, 0);
        char t[256];
        snprintf(t, sizeof(t), "%s (%s) 请求加你为好友\n附言: %s\n请到左侧 通知 页处理",
                 m->from_nick, m->from_name, m->body);
        ui_notify_text("好友申请", t);
        break;
    }
    case MSG_GROUP_JOIN_NOTIFY: {
        ui_add_request(1, (int)m->status, m->from_name, m->from_nick, (int)m->group_id % 10,
                       m->to_name, m->body, (int)m->group_id);
        char t[256];
        snprintf(t, sizeof(t), "%s 申请加入群【%s】\n附言: %s\n请到左侧 通知 页处理",
                 m->from_nick, m->to_name, m->body);
        ui_notify_text("入群申请", t);
        break;
    }
    case MSG_FRIEND_REQ_LIST:
        ui_refresh_requests(0, m->body);
        break;
    case MSG_GROUP_JOIN_REQ_LIST:
        ui_refresh_requests(1, m->body);
        break;

    case MSG_USER_SEARCH:
        ui_search_result(1, m->body);
        break;
    case MSG_GROUP_SEARCH:
        ui_search_result(0, m->body);
        break;

    default: break;
    }
    g_free(p);
    return G_SOURCE_REMOVE;
}

/* 接收线程: 死循环 recv, 每条消息交给主线程处理.
 * 还需要处理 MSG_FILE_CHUNK 的写盘 — 它不需要 UI, 直接在本线程做. */
void *recv_thread(void *arg) {
    (void)arg;
    Message m;
    FILE *recv_fp = NULL;
    char  recv_name[128] = {0};
    mkdir("recv", 0755);
    while (recv_msg(CTX.sockfd, &m) == 0) {
        if (m.type == MSG_FILE_BEGIN) {
            snprintf(recv_name, sizeof(recv_name), "recv/%s_%s", m.from_name, m.body);
            if (recv_fp) fclose(recv_fp);
            recv_fp = fopen(recv_name, "wb");
        } else if (m.type == MSG_FILE_CHUNK) {
            if (recv_fp) fwrite(m.body, 1, m.body_len, recv_fp);
            continue;            /* 不打扰 UI */
        } else if (m.type == MSG_FILE_END) {
            if (recv_fp) { fclose(recv_fp); recv_fp = NULL; }
        }
        DispatchPack *p = g_malloc(sizeof(*p));
        p->m = m;
        g_idle_add(dispatch_in_main, p);
    }
    if (recv_fp) fclose(recv_fp);
    return NULL;
}
