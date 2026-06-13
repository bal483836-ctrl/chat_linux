#ifndef CHAT_NET_IO_H
#define CHAT_NET_IO_H

#include "protocol.h"

/* 阻塞式完整收发一个 Message. 失败返回 -1, 否则返回 0. */
int send_msg(int fd, const Message *m);
int recv_msg(int fd, Message *m);

/* 填充当前时间 "YYYY-MM-DD HH:MM:SS" */
void fill_timestamp(char *buf, int len);

#endif
