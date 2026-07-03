#include "net_io.h"
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <errno.h>

/* 循环把 len 字节完整读满 / 写完的核心函数, 解决 TCP 的"半包"问题。
 *
 * 背景: 一次 read/write 不保证搬完全部字节(尤其大消息), 返回值是"本次实际
 * 搬了多少"。所以必须循环: 用 left 记还剩多少, p 记当前进度指针, 每次搬完
 * 就把 left 减少、p 前移, 直到 left 归零。
 *
 * 三种返回情况:
 *   n == 0  : 读时表示对端已关闭连接(EOF) -> 返回 -1;
 *   n <  0  : 出错; 若是 EINTR(被信号中断)则 continue 重试, 否则真错 -> -1;
 *   n >  0  : 正常搬了 n 字节, 更新 left/p 继续。
 * do_read 复用同一套逻辑: 1 走 read, 0 走 write。 */
static int io_all(int fd, void *buf, size_t len, int do_read) {
    size_t left = len;             /* 还需搬运的字节数 */
    char *p = (char *)buf;         /* 当前搬运位置 */
    while (left > 0) {
        ssize_t n = do_read ? read(fd, p, left) : write(fd, p, left);
        if (n == 0) return -1;                 /* 对端关闭 */
        if (n < 0) {
            if (errno == EINTR) continue;      /* 被信号打断, 不算错, 重试 */
            return -1;                         /* 其他错误 */
        }
        left -= (size_t)n;                     /* 减去已搬部分 */
        p    += n;                             /* 指针前移 */
    }
    return 0;                                  /* 全部搬完 */
}

/* 发送一个完整 Message(定长 sizeof(Message) 字节)。0 成功 / -1 失败。 */
int send_msg(int fd, const Message *m) {
    return io_all(fd, (void *)m, sizeof(*m), 0);
}

/* 接收一个完整 Message。因为收发都是定长, 无需额外的长度前缀协议。 */
int recv_msg(int fd, Message *m) {
    return io_all(fd, m, sizeof(*m), 1);
}

/* 把当前本地时间格式化成 "YYYY-MM-DD HH:MM:SS" 填进 buf。
 * 用 localtime_r(可重入版, 线程安全)而非 localtime, 因为服务器是多线程的。 */
void fill_timestamp(char *buf, int len) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm);
}
