/* =========================================================
 *  chat_linux 服务器 - 并发 IM Server
 *
 *  支持三种可切换的并发模型 (环境变量 CHAT_MODE):
 *
 *    thread      每来一个连接就 pthread_create 一个工作线程 (默认, 原始实现)
 *    threadpool  单进程 + 固定大小线程池, accept 后把 fd 投进任务队列
 *    process     pre-fork 进程池, 每个子进程各自连库 + 自带线程池 accept
 *
 *  架构 (thread / threadpool):
 *    - 主线程: socket() / bind() / listen() / accept() 循环
 *    - 在线用户表 g_tab 由互斥锁 g_mu 保护 (online.c)
 *    - MySQL 连接由 g_dbmu 串行化 (db.c)
 *    - SIGPIPE 屏蔽, 避免对端断开后写 socket 把整个进程打死
 *
 *  用法:
 *    ./chat_server [port]
 *  环境变量:
 *    CHAT_MODE     thread | threadpool | process   (默认 thread)
 *    CHAT_THREADS  线程池线程数                     (默认 = CPU 核数 * 2)
 *    CHAT_WORKERS  进程池子进程数 (仅 process 模式) (默认 = CPU 核数)
 *    CHAT_DB_HOST  (默认 127.0.0.1)
 *    CHAT_DB_USER  (默认 root)
 *    CHAT_DB_PASS  (默认 "")
 *    CHAT_DB_NAME  (默认 chat_linux)
 *
 *  ⚠ process 模式下各子进程在线表不共享, 连到不同子进程的用户无法实时互推
 *    (详见 procpool.h)。需要完整实时多用户聊天请用 thread / threadpool。
 * ========================================================= */
#include "../common/protocol.h"
#include "handler.h"
#include "online.h"
#include "db.h"
#include "threadpool.h"
#include "procpool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* 并发模型 */
enum RunMode { MODE_THREAD, MODE_THREADPOOL, MODE_PROCESS };

/* 线程池任务队列容量 (排队等待处理的连接数上限) */
#define POOL_QUEUE_CAP  256

static volatile int g_running = 1;
static int g_listen_fd = -1;

static void on_sigint(int sig) { (void)sig; g_running = 0; if (g_listen_fd >= 0) close(g_listen_fd); }

/* 读正整数环境变量, 不存在或非法时回退 def。 */
static int env_int(const char *name, int def) {
    const char *e = getenv(name);
    if (e) { int v = atoi(e); if (v > 0) return v; }
    return def;
}

static int cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 4;
}

static enum RunMode parse_mode(void) {
    const char *m = getenv("CHAT_MODE");
    if (m && strcmp(m, "threadpool") == 0) return MODE_THREADPOOL;
    if (m && strcmp(m, "process")    == 0) return MODE_PROCESS;
    return MODE_THREAD;   /* 默认: 每连接一线程 (向后兼容) */
}

/* 创建 + 绑定 + 监听 socket。失败返回 -1。 */
static int make_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in sa = {0};
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { perror("bind"); close(fd); return -1; }
    if (listen(fd, 64) < 0)                              { perror("listen"); close(fd); return -1; }
    return fd;
}

/* thread / threadpool 模式共用的 accept 循环。tp 为 NULL 时是"每连接一线程"。 */
static void accept_loop(threadpool_t *tp) {
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

        if (tp) {
            /* 线程池模式: 把连接投进任务队列, 队列满则阻塞(背压) */
            if (threadpool_submit(tp, cfd) != 0) close(cfd);
        } else {
            /* 每连接一线程模式 */
            int *arg = malloc(sizeof(int));
            *arg = cfd;
            pthread_t tid;
            if (pthread_create(&tid, NULL, client_thread, arg) != 0) {
                perror("pthread_create");
                close(cfd);
                free(arg);
            }
        }
    }
}

int main(int argc, char **argv) {
    int port = SERVER_PORT;
    if (argc >= 2) port = atoi(argv[1]);

    /* 行缓冲: 否则 stdout 重定向到管道时是全缓冲, process 模式 fork 会把
     * 未刷新的缓冲一并复制给子进程, 造成日志重复打印。 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* 屏蔽 SIGPIPE: 写到已关闭 socket 时不会让进程 abort */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  on_sigint);
    signal(SIGTERM, on_sigint);

    const char *h = getenv("CHAT_DB_HOST"); if (!h) h = "127.0.0.1";
    const char *u = getenv("CHAT_DB_USER"); if (!u) u = "root";
    const char *p = getenv("CHAT_DB_PASS"); if (!p) p = "";
    const char *d = getenv("CHAT_DB_NAME"); if (!d) d = "chat_linux";

    enum RunMode mode = parse_mode();

    /* 监听 socket 先建好: process 模式要在 fork 之前建, 让所有子进程继承同一 fd */
    g_listen_fd = make_listen_socket(port);
    if (g_listen_fd < 0) return 1;

    /* ---------- 进程池模式: master 不连库, 各子进程独立 db_init ---------- */
    if (mode == MODE_PROCESS) {
        int nworkers = env_int("CHAT_WORKERS", cpu_count());
        int nthreads = env_int("CHAT_THREADS", cpu_count() * 2);
        printf("[chat_server] mode=process, listening on 0.0.0.0:%d\n", port);
        procpool_run(g_listen_fd, nworkers, nthreads, POOL_QUEUE_CAP, h, u, p, d);
        close(g_listen_fd);
        return 0;
    }

    /* ---------- 单进程模式 (thread / threadpool): master 自己连库 ---------- */
    if (db_init(h, u, p, d) < 0) {
        fprintf(stderr, "[FATAL] db_init failed\n");
        return 1;
    }
    online_init();

    threadpool_t *tp = NULL;
    if (mode == MODE_THREADPOOL) {
        int nthreads = env_int("CHAT_THREADS", cpu_count() * 2);
        /* 创建线程池前先在本线程屏蔽 INT/TERM, 让工作线程继承这个屏蔽掩码,
         * 从而保证退出信号只投递到阻塞在 accept() 的主线程 -> 能被 EINTR 唤醒。
         * 否则信号可能被某个工作线程"吃掉", 主线程的 accept() 永远不返回。 */
        sigset_t block, old;
        sigemptyset(&block);
        sigaddset(&block, SIGINT);
        sigaddset(&block, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &block, &old);
        tp = threadpool_create(nthreads, POOL_QUEUE_CAP);
        pthread_sigmask(SIG_SETMASK, &old, NULL);
        if (!tp) {
            fprintf(stderr, "[FATAL] threadpool_create failed\n");
            online_destroy();
            db_close();
            return 1;
        }
        printf("[chat_server] mode=threadpool (threads=%d), listening on 0.0.0.0:%d\n",
               nthreads, port);
    } else {
        printf("[chat_server] mode=thread, listening on 0.0.0.0:%d\n", port);
    }

    accept_loop(tp);

    printf("[chat_server] shutting down\n");
    if (tp) threadpool_destroy(tp);
    online_destroy();
    db_close();
    return 0;
}
