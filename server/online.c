#include "online.h"
#include "../common/net_io.h"
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

static OnlineEntry     g_tab[MAX_ONLINE_USERS];
static int             g_cnt = 0;
static pthread_mutex_t g_mu  = PTHREAD_MUTEX_INITIALIZER;

void online_init(void)    { memset(g_tab, 0, sizeof(g_tab)); g_cnt = 0; }
void online_destroy(void) { pthread_mutex_destroy(&g_mu); }

int online_add(int user_id, const char *name, int fd) {
    pthread_mutex_lock(&g_mu);
    /* 同名挤掉旧连接 */
    for (int i = 0; i < g_cnt; ++i) {
        if (g_tab[i].user_id == user_id) {
            close(g_tab[i].sockfd);
            g_tab[i].sockfd = fd;
            pthread_mutex_unlock(&g_mu);
            return 0;
        }
    }
    if (g_cnt >= MAX_ONLINE_USERS) { pthread_mutex_unlock(&g_mu); return -1; }
    g_tab[g_cnt].user_id = user_id;
    g_tab[g_cnt].sockfd  = fd;
    strncpy(g_tab[g_cnt].username, name, MAX_NAME_LEN - 1);
    g_cnt++;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

void online_remove_by_fd(int fd) {
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_cnt; ++i) {
        if (g_tab[i].sockfd == fd) {
            g_tab[i] = g_tab[--g_cnt];
            break;
        }
    }
    pthread_mutex_unlock(&g_mu);
}

int online_get_fd_by_name(const char *name) {
    int fd = -1;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_cnt; ++i) {
        if (strcmp(g_tab[i].username, name) == 0) { fd = g_tab[i].sockfd; break; }
    }
    pthread_mutex_unlock(&g_mu);
    return fd;
}

int online_get_fd_by_id(int user_id) {
    int fd = -1;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_cnt; ++i) {
        if (g_tab[i].user_id == user_id) { fd = g_tab[i].sockfd; break; }
    }
    pthread_mutex_unlock(&g_mu);
    return fd;
}

int online_push(int user_id, const Message *m) {
    int fd = online_get_fd_by_id(user_id);
    if (fd < 0) return -1;
    /* 注意: send_msg 在 fd 上做完整 write. 由于多线程都可能往同一 fd 写
     * (例如同时有人发私聊和群聊), 这里加锁串行化写入. */
    static pthread_mutex_t write_mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&write_mu);
    int r = send_msg(fd, m);
    pthread_mutex_unlock(&write_mu);
    return r;
}

int online_broadcast(const int *ids, int n, const Message *m, int except_id) {
    int delivered = 0;
    for (int i = 0; i < n; ++i) {
        if (ids[i] == except_id) continue;
        if (online_push(ids[i], m) == 0) delivered++;
    }
    return delivered;
}
