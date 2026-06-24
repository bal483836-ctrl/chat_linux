#ifndef CHAT_THREADPOOL_H
#define CHAT_THREADPOOL_H

/* =========================================================
 *  连接处理线程池 (thread pool)
 *
 *  动机:
 *    原始实现"每来一个连接就 pthread_create 一个线程", 在高并发/短连接
 *    风暴下会频繁创建销毁线程, 且并发线程数不受控, 可能耗尽内存或调度
 *    资源。线程池预先创建固定数量的工作线程, 主线程 accept 后只把 client
 *    fd 投进任务队列, 工作线程取出并调用 serve_client() 处理整条连接的
 *    生命周期。这样:
 *      - 复用线程, 省去反复 create/join 的开销
 *      - 用队列容量给并发施加上限 (背压), 防止连接风暴打爆系统
 *
 *  队列满时 threadpool_submit 会阻塞等待, 形成自然的背压。
 * ========================================================= */

typedef struct threadpool threadpool_t;

/* 创建线程池: nthreads 个工作线程, 任务队列容量 queue_cap (单位: 连接数)。
 * 失败返回 NULL。 */
threadpool_t *threadpool_create(int nthreads, int queue_cap);

/* 投递一个待处理的 client fd 给线程池。
 * 队列满时阻塞直到有空位 (背压)。成功返回 0; 池已在关闭中返回 -1。 */
int threadpool_submit(threadpool_t *tp, int client_fd);

/* 优雅关闭: 停止接收新任务, 把队列里剩余的连接处理完, 再回收所有线程。 */
void threadpool_destroy(threadpool_t *tp);

#endif
