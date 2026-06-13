#include "net_io.h"
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <errno.h>

/* 处理 short read/write 的循环 IO. EINTR 重试. */
static int io_all(int fd, void *buf, size_t len, int do_read) {
    size_t left = len;
    char *p = (char *)buf;
    while (left > 0) {
        ssize_t n = do_read ? read(fd, p, left) : write(fd, p, left);
        if (n == 0) return -1;                 /* 对端关闭 */
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        left -= (size_t)n;
        p    += n;
    }
    return 0;
}

int send_msg(int fd, const Message *m) {
    return io_all(fd, (void *)m, sizeof(*m), 0);
}

int recv_msg(int fd, Message *m) {
    return io_all(fd, m, sizeof(*m), 1);
}

void fill_timestamp(char *buf, int len) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &tm);
}
