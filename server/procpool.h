#ifndef CHAT_PROCPOOL_H
#define CHAT_PROCPOOL_H

/* =========================================================
 *  进程池 (pre-fork 进程池模型)
 *
 *  master 进程先建好并 listen 监听 socket, 再 fork 出 nworkers 个子进程。
 *  每个子进程各自:
 *    - 独立 db_init() 连接 MySQL (MySQL 连接句柄不能跨 fork 共享)
 *    - 初始化自己的在线表 online_init()
 *    - 用一个线程池 (threadpool) 在共享的 listen_fd 上 accept 并处理连接
 *  多个子进程同时 accept 同一个监听 fd, 由内核在它们之间做负载均衡
 *  (Linux 会唤醒其中一个, 新内核基本无"惊群")。
 *
 *  ┌──────── master ────────┐
 *  │ socket/bind/listen      │
 *  │ fork × N                │
 *  └─┬───────┬───────┬───────┘
 *    │       │       │
 *  worker  worker  worker      每个 worker = 1 进程 + 1 线程池
 *
 *  ⚠ 局限 (教学项目须知):
 *    各子进程地址空间独立, 在线表和 client socket 都不共享。因此连接到
 *    *不同* 子进程的两个用户无法互相实时推送消息 (离线消息经 MySQL 仍可达,
 *    重新登录即可收到)。生产级方案需要共享内存 / 独立的消息路由进程 /
 *    消息队列来跨进程投递。本模式主要用于演示 pre-fork 并发模型本身。
 *    若要完整的实时多用户聊天, 请用 thread / threadpool 单进程模式。
 *
 *  procpool_run 会阻塞直到收到 SIGINT/SIGTERM, 通知并回收所有子进程后返回。
 * ========================================================= */

int procpool_run(int listen_fd, int nworkers,
                 int pool_threads, int pool_queue,
                 const char *db_host, const char *db_user,
                 const char *db_pass, const char *db_name);

#endif
