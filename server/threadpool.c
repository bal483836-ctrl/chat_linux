#include "threadpool.h"
#include "handler.h"

#include <pthread.h>
#include <stdlib.h>

/* 线程池内部状态。
 * 任务队列是一个存放 client fd 的环形缓冲, 由 mu 保护;
 * not_empty / not_full 两个条件变量实现"生产者-消费者"的阻塞同步。 */
struct threadpool {
    pthread_mutex_t mu;
    pthread_cond_t  not_empty;   /* 队列非空 -> 唤醒工作线程取任务 */
    pthread_cond_t  not_full;    /* 队列非满 -> 唤醒投递者继续投递 */

    int            *queue;       /* 环形缓冲, 元素是 client fd */
    int             cap;         /* 队列容量 */
    int             head;        /* 出队位置 */
    int             tail;        /* 入队位置 */
    int             count;       /* 当前排队任务数 */

    pthread_t      *workers;     /* 工作线程句柄数组 */
    int             nthreads;    /* 已成功创建的工作线程数 */
    int             shutdown;    /* 1 = 不再收新任务, 排空后退出 */
};

/* 工作线程主循环: 从队列取出一个 client fd, 完整处理这条连接。 */
static void *worker_main(void *arg) {
    threadpool_t *tp = (threadpool_t *)arg;
    for (;;) {
        pthread_mutex_lock(&tp->mu);
        while (tp->count == 0 && !tp->shutdown)
            pthread_cond_wait(&tp->not_empty, &tp->mu);
        /* 收到关闭信号且队列已排空 -> 退出 */
        if (tp->count == 0 && tp->shutdown) {
            pthread_mutex_unlock(&tp->mu);
            break;
        }
        int fd = tp->queue[tp->head];
        tp->head = (tp->head + 1) % tp->cap;
        tp->count--;
        pthread_cond_signal(&tp->not_full);
        pthread_mutex_unlock(&tp->mu);

        /* serve_client 负责处理整条连接并在结束时 close(fd) */
        serve_client(fd);
    }
    return NULL;
}

threadpool_t *threadpool_create(int nthreads, int queue_cap) {
    if (nthreads <= 0 || queue_cap <= 0) return NULL;

    threadpool_t *tp = calloc(1, sizeof(*tp));
    if (!tp) return NULL;
    tp->queue   = malloc(sizeof(int) * (size_t)queue_cap);
    tp->workers = malloc(sizeof(pthread_t) * (size_t)nthreads);
    if (!tp->queue || !tp->workers) {
        free(tp->queue);
        free(tp->workers);
        free(tp);
        return NULL;
    }
    tp->cap = queue_cap;
    pthread_mutex_init(&tp->mu, NULL);
    pthread_cond_init(&tp->not_empty, NULL);
    pthread_cond_init(&tp->not_full, NULL);

    for (int i = 0; i < nthreads; ++i) {
        if (pthread_create(&tp->workers[i], NULL, worker_main, tp) != 0) {
            /* 只回收已建好的线程后销毁整个池 */
            tp->nthreads = i;
            threadpool_destroy(tp);
            return NULL;
        }
    }
    tp->nthreads = nthreads;
    return tp;
}

int threadpool_submit(threadpool_t *tp, int client_fd) {
    pthread_mutex_lock(&tp->mu);
    while (tp->count == tp->cap && !tp->shutdown)
        pthread_cond_wait(&tp->not_full, &tp->mu);
    if (tp->shutdown) {
        pthread_mutex_unlock(&tp->mu);
        return -1;
    }
    tp->queue[tp->tail] = client_fd;
    tp->tail = (tp->tail + 1) % tp->cap;
    tp->count++;
    pthread_cond_signal(&tp->not_empty);
    pthread_mutex_unlock(&tp->mu);
    return 0;
}

void threadpool_destroy(threadpool_t *tp) {
    if (!tp) return;

    pthread_mutex_lock(&tp->mu);
    tp->shutdown = 1;
    pthread_cond_broadcast(&tp->not_empty);   /* 唤醒所有等任务的工作线程 */
    pthread_cond_broadcast(&tp->not_full);    /* 唤醒被背压阻塞的投递者 */
    pthread_mutex_unlock(&tp->mu);

    for (int i = 0; i < tp->nthreads; ++i)
        pthread_join(tp->workers[i], NULL);

    pthread_mutex_destroy(&tp->mu);
    pthread_cond_destroy(&tp->not_empty);
    pthread_cond_destroy(&tp->not_full);
    free(tp->queue);
    free(tp->workers);
    free(tp);
}
