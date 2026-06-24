#include "procpool.h"
#include "threadpool.h"
#include "handler.h"
#include "online.h"
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/socket.h>

#define MAX_WORKERS 256

/* 安装信号处理函数, 关键: sa_flags 不带 SA_RESTART, 这样 accept()/waitpid()
 * 被信号打断时返回 EINTR 而不是自动重启, 我们才有机会检查退出标志并收尾。
 * (glibc 的 signal() 默认带 SA_RESTART, 会导致系统调用自动重启 -> 卡死) */
static void install_handler(int sig, void (*fn)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(sig, &sa, NULL);
}

/* ---------- 子进程侧 ---------- */

static volatile sig_atomic_t g_child_run = 1;
static int                   g_child_listen_fd = -1;

/* 收到退出信号: 置位并关闭监听 fd, 让阻塞中的 accept() 立即返回。 */
static void child_on_stop(int sig) {
    (void)sig;
    g_child_run = 0;
    if (g_child_listen_fd >= 0) close(g_child_listen_fd);
}

/* 子进程主体: 独立连库 + 自己的线程池, 在 listen_fd 上 accept 循环。 */
static void child_loop(int listen_fd, int pool_threads, int pool_queue,
                       const char *h, const char *u, const char *p, const char *d) {
    /* 子进程重装信号: INT/TERM 时停掉 accept 循环, 优雅退出 */
    g_child_listen_fd = listen_fd;
    install_handler(SIGINT,  child_on_stop);
    install_handler(SIGTERM, child_on_stop);
    signal(SIGPIPE, SIG_IGN);

    if (db_init(h, u, p, d) < 0) {
        fprintf(stderr, "[worker %d] db_init failed\n", (int)getpid());
        _exit(1);
    }
    online_init();

    /* 创建线程池前屏蔽 INT/TERM, 工作线程继承该掩码; 这样退出信号只会投递到
     * 阻塞在 accept() 的子进程主线程, 使 accept() 被 EINTR 唤醒后能正常退出。
     * 否则信号可能被某个工作线程接走, 主线程一直卡在 accept(), 进程无法退出。 */
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    threadpool_t *tp = threadpool_create(pool_threads, pool_queue);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (!tp) {
        fprintf(stderr, "[worker %d] threadpool_create failed\n", (int)getpid());
        db_close();
        _exit(1);
    }

    printf("[worker %d] ready (pool threads=%d)\n", (int)getpid(), pool_threads);
    fflush(stdout);

    while (g_child_run) {
        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;   /* 信号打断, 回到 while 判断退出 */
            continue;
        }
        threadpool_submit(tp, cfd);
    }

    threadpool_destroy(tp);   /* 排空在处理的连接 */
    db_close();
    _exit(0);
}

/* ---------- master 侧 ---------- */

static pid_t              g_pids[MAX_WORKERS];
static int                g_npids = 0;
static volatile sig_atomic_t g_master_stop = 0;

static void master_on_stop(int sig) { (void)sig; g_master_stop = 1; }

int procpool_run(int listen_fd, int nworkers,
                 int pool_threads, int pool_queue,
                 const char *h, const char *u, const char *p, const char *d) {
    if (nworkers <= 0) nworkers = 1;
    if (nworkers > MAX_WORKERS) nworkers = MAX_WORKERS;

    for (int i = 0; i < nworkers; ++i) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); break; }
        if (pid == 0) {
            child_loop(listen_fd, pool_threads, pool_queue, h, u, p, d);
            _exit(0);   /* child_loop 内部已 _exit, 兜底 */
        }
        g_pids[g_npids++] = pid;
    }

    if (g_npids == 0) {
        fprintf(stderr, "[chat_server] process pool: no worker spawned\n");
        return -1;
    }

    /* master 不参与 accept, 只看护子进程。重装信号 (无 SA_RESTART) 以便
     * waitpid 被打断后能检查停止标志, 进而通知并回收子进程。 */
    install_handler(SIGINT,  master_on_stop);
    install_handler(SIGTERM, master_on_stop);

    printf("[chat_server] process pool: %d workers x %d threads each\n",
           g_npids, pool_threads);
    fflush(stdout);

    /* 看护循环: 任一子进程退出就回收; 收到停止信号则跳出去统一收尾。
     * 注意终端 Ctrl-C 会把 SIGINT 投给整个前台进程组, 子进程也会各自收到。 */
    int alive = g_npids;
    while (alive > 0 && !g_master_stop) {
        int status;
        pid_t w = waitpid(-1, &status, 0);
        if (w > 0) { alive--; continue; }
        if (errno == EINTR) continue;     /* 被信号打断 -> 重判循环条件 */
        break;
    }

    /* 通知所有还活着的子进程退出, 然后回收, 避免僵尸进程。 */
    for (int i = 0; i < g_npids; ++i)
        if (g_pids[i] > 0) kill(g_pids[i], SIGTERM);

    for (;;) {
        int status;
        pid_t w = waitpid(-1, &status, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;    /* ECHILD: 已无子进程 */
        }
    }

    printf("[chat_server] process pool: all workers reaped\n");
    return 0;
}
