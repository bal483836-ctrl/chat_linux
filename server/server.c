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

static volatile int g_running = 1;   /* 主循环开关; volatile 因信号处理函数会改它 */
static int g_listen_fd = -1;         /* 监听 socket, 关它可唤醒阻塞中的 accept */

/* SIGINT/SIGTERM 处理: 置停机标志并关掉监听 fd, 让 accept 立刻返回从而退出主循环。
 * 注意信号处理里能安全做的事很有限, 这里只做最小动作。 */
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

    /* 建立监听 socket 的标准四步: socket -> setsockopt -> bind -> listen */
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);   /* IPv4 + TCP */
    if (g_listen_fd < 0) { perror("socket"); return 1; }

    /* SO_REUSEADDR: 允许重启后立刻重新绑定同一端口(否则 TIME_WAIT 期间会 bind 失败) */
    int yes = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in sa = {0};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);          /* 监听所有网卡地址 */
    sa.sin_port        = htons(port);                /* htons: 主机字节序->网络字节序 */

    if (bind(g_listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { perror("bind"); return 1; }
    if (listen(g_listen_fd, 64) < 0)                              { perror("listen"); return 1; }  /* 64=未完成连接队列长度 */

    printf("[chat_server] listening on 0.0.0.0:%d\n", port);

    /* 主循环: 不断 accept 新连接, 每个连接交给一个独立线程处理 */
    while (g_running) {
        struct sockaddr_in ca;                       /* 存放客户端地址 */
        socklen_t cl = sizeof(ca);
        int cfd = accept(g_listen_fd, (struct sockaddr *)&ca, &cl);   /* 阻塞等新连接 */
        if (cfd < 0) {
            if (!g_running) break;                   /* 是停机信号关了 fd 导致的返回 -> 退出 */
            continue;                                /* 其他偶发错误 -> 继续等 */
        }
        printf("[chat_server] accept fd=%d from %s:%d\n",
               cfd, inet_ntoa(ca.sin_addr), ntohs(ca.sin_port));

        /* 把 cfd 放到堆上传给线程(不能传 &cfd, 它下轮循环就变了)。
         * 线程内部负责 free 这块内存。 */
        int *arg = malloc(sizeof(int));
        *arg = cfd;
        pthread_t tid;
        if (pthread_create(&tid, NULL, client_thread, arg) != 0) {
            perror("pthread_create");
            close(cfd);                              /* 起线程失败, 回收资源 */
            free(arg);
        }
    }

    printf("[chat_server] shutting down\n");
    online_destroy();
    db_close();
    return 0;
}
