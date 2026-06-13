/* =========================================================
 *  chat_linux 服务器 - 多线程并发 IM Server
 *
 *  架构:
 *    - 主线程: socket() / bind() / listen() / accept() 循环
 *    - 每来一个客户端 -> pthread_create 一个工作线程 (client_thread)
 *    - 在线用户表 g_tab 由互斥锁 g_mu 保护 (online.c)
 *    - MySQL 连接由 g_dbmu 串行化 (db.c)
 *    - SIGPIPE 屏蔽, 避免对端断开后写 socket 整个进程被打死
 *
 *  用法:
 *    ./chat_server [port]
 *  环境变量:
 *    CHAT_DB_HOST  (默认 127.0.0.1)
 *    CHAT_DB_USER  (默认 root)
 *    CHAT_DB_PASS  (默认 "")
 *    CHAT_DB_NAME  (默认 chat_linux)
 * ========================================================= */
#include "../common/protocol.h"
#include "handler.h"
#include "online.h"
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static volatile int g_running = 1;
static int g_listen_fd = -1;

static void on_sigint(int sig) { (void)sig; g_running = 0; if (g_listen_fd >= 0) close(g_listen_fd); }

int main(int argc, char **argv) {
    int port = SERVER_PORT;
    if (argc >= 2) port = atoi(argv[1]);

    /* 屏蔽 SIGPIPE: 写到已关闭 socket 时不会让进程 abort */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  on_sigint);
    signal(SIGTERM, on_sigint);

    const char *h = getenv("CHAT_DB_HOST"); if (!h) h = "127.0.0.1";
    const char *u = getenv("CHAT_DB_USER"); if (!u) u = "root";
    const char *p = getenv("CHAT_DB_PASS"); if (!p) p = "";
    const char *d = getenv("CHAT_DB_NAME"); if (!d) d = "chat_linux";
    if (db_init(h, u, p, d) < 0) {
        fprintf(stderr, "[FATAL] db_init failed\n");
        return 1;
    }
    online_init();

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in sa = {0};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port        = htons(port);

    if (bind(g_listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { perror("bind"); return 1; }
    if (listen(g_listen_fd, 64) < 0)                              { perror("listen"); return 1; }

    printf("[chat_server] listening on 0.0.0.0:%d\n", port);

    while (g_running) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int cfd = accept(g_listen_fd, (struct sockaddr *)&ca, &cl);
        if (cfd < 0) {
            if (!g_running) break;
            continue;
        }
        printf("[chat_server] accept fd=%d from %s:%d\n",
               cfd, inet_ntoa(ca.sin_addr), ntohs(ca.sin_port));

        int *arg = malloc(sizeof(int));
        *arg = cfd;
        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, arg) != 0) {
            perror("pthread_create");
            close(cfd);
            free(arg);
        }
    }

    printf("[chat_server] shutting down\n");
    online_destroy();
    db_close();
    return 0;
}
