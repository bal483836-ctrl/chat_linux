#include "client.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    memset(&CTX, 0, sizeof(CTX));
    CTX.sockfd = -1;
    show_login(argc, argv);
    net_close();
    return 0;
}
