#include "client.h"
#include "../common/net_io.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>

/* Ctrl+C / kill 信号: 优雅退出 (功能 5).
 * 不能在信号处理里直接走 GTK 函数 (GTK 不是异步信号安全), 所以这里
 * 只做两件不可重入风险最小的事:
 *   - 给服务器发 MSG_LOGOUT (单次 write, 失败也无所谓)
 *   - 关 socket; 接收线程在 recv 上的阻塞会立刻 EBADF/0 返回从而退出
 * 真正的 GTK 退出留给主线程: 我们在主循环里检测 net_close 后会自然走完. */
static void on_term_signal(int sig) {
    (void)sig;
    if (CTX.sockfd > 0) {
        Message bye; memset(&bye, 0, sizeof(bye));
        bye.type = MSG_LOGOUT;
        send_msg(CTX.sockfd, &bye);
        net_close();
    }
    /* 触发 GTK 主循环退出 */
    gtk_main_quit();
}

int main(int argc, char **argv) {
    /* SIGPIPE 必须屏蔽: 写已经关闭的 socket 会让进程直接 abort */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  on_term_signal);
    signal(SIGTERM, on_term_signal);

    memset(&CTX, 0, sizeof(CTX));
    CTX.sockfd = -1;
    show_login(argc, argv);
    /* gtk_main 退出后再保险关一次 (双 close 无害) */
    net_close();
    return 0;
}
