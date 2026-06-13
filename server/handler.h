#ifndef CHAT_HANDLER_H
#define CHAT_HANDLER_H

/* 每个客户端连接的处理线程入口. arg = int* (socket fd, 由本函数 free). */
void *client_thread(void *arg);

#endif
