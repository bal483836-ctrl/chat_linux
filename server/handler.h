#ifndef CHAT_HANDLER_H
#define CHAT_HANDLER_H

/* 处理一条客户端连接的完整生命周期 (收消息->分发->应答的循环), 直到对端
 * 断开或退出登录。函数返回前会 close(fd)。
 *
 * 线程池 / 进程池模型直接调用本函数 (在自己的工作线程里), 而不必关心 fd
 * 的内存管理。 */
void serve_client(int fd);

/* 每个客户端连接的处理线程入口 (用于"每连接一线程"模式)。
 * arg = int* (socket fd, 由本函数 free)。内部 detach 后调用 serve_client。 */
void *client_thread(void *arg);

#endif
