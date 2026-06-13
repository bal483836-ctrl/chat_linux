#ifndef CHAT_ONLINE_H
#define CHAT_ONLINE_H

#include "../common/protocol.h"

/* 在线用户表. 多线程共享, 使用互斥锁保护. */
typedef struct {
    int  user_id;
    int  sockfd;
    char username[MAX_NAME_LEN];
} OnlineEntry;

void online_init(void);
void online_destroy(void);

/* 添加. 若该用户已在线, 旧连接被关闭并替换. */
int  online_add(int user_id, const char *name, int fd);
/* 通过 fd 移除. */
void online_remove_by_fd(int fd);
/* 查询: 返回 socket fd, 若不在线返回 -1. */
int  online_get_fd_by_name(const char *name);
int  online_get_fd_by_id(int user_id);
/* 推送给某用户. 不在线返回 -1, 调用方应入离线队列. */
int  online_push(int user_id, const Message *m);
/* 广播给一组在线用户. ids[] 长度 n. 返回成功投递数. */
int  online_broadcast(const int *ids, int n, const Message *m, int except_id);

#endif
