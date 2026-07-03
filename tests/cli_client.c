/* 简易命令行客户端: 用同一协议与 chat_server 交互, 方便自动化测试.
 * 用法: cli_client <host> <port>
 * stdin 行:
 *     REG  user pass
 *     LOGIN user pass
 *     PMSG  to text...
 *     GMSG  gid text...
 *     FADD  name
 *     FDEL  name
 *     BADD  name
 *     BDEL  name
 *     FLIST
 *     GCREATE name
 *     GJOIN gid
 *     GLIST
 *     HISTP name
 *     HISTG gid
 *     QUIT
 * 后台线程将收到的消息打印到 stdout, 前缀 "<<". */
#include "../common/protocol.h"
#include "../common/net_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int g_fd = -1;
static pthread_mutex_t wmu = PTHREAD_MUTEX_INITIALIZER;

static void send_all(const Message *m) {
    pthread_mutex_lock(&wmu); send_msg(g_fd, m); pthread_mutex_unlock(&wmu);
}

static const char *type_name(int t) {
    switch (t) {
    case MSG_RESPONSE:       return "RESP";
    case MSG_PRIVATE_CHAT:   return "PCHAT";
    case MSG_GROUP_CHAT:     return "GCHAT";
    case MSG_FRIEND_LIST:    return "FLIST";
    case MSG_GROUP_LIST:     return "GLIST";
    case MSG_NOTIFY_ONLINE:  return "ONLINE";
    case MSG_NOTIFY_OFFLINE: return "OFFLINE";
    case MSG_FILE_BEGIN:     return "FBEGIN";
    case MSG_FILE_CHUNK:     return "FCHUNK";
    case MSG_FILE_END:       return "FEND";
    default:                 return "OTHER";
    }
}

/* 后台接收线程: 循环收消息并单行打印到 stdout, 便于测试脚本 grep 断言。
 * body 里的换行替换成 '|' 以免破坏"一条消息一行"的格式; 最多截取 250 字节。 */
static void *recv_th(void *_) {
    (void)_;
    Message m;
    while (recv_msg(g_fd, &m) == 0) {
        char body[256] = {0};
        int n = m.body_len < 250 ? m.body_len : 250;   /* 限长, 防超长 body 刷屏 */
        memcpy(body, m.body, n);
        for (int i = 0; i < n; ++i) if (body[i] == '\n') body[i] = '|';   /* 换行->竖线 */
        printf("<< type=%-7s status=%u group=%u from=%s to=%s body=\"%s\"\n",
               type_name(m.type), m.status, m.group_id,
               m.from_name, m.to_name, body);
        fflush(stdout);   /* 立即刷出, 保证测试能实时读到 */
    }
    return NULL;
}

static void cmd_login(int reg, char *u, char *p) {
    Message m; memset(&m, 0, sizeof(m));
    m.type = reg ? MSG_REGISTER : MSG_LOGIN;
    snprintf(m.body, sizeof(m.body), "%s\n%s", u, p);
    m.body_len = strlen(m.body);
    send_all(&m);
}
static void cmd_text(int type, const char *to, int gid, const char *txt) {
    Message m; memset(&m, 0, sizeof(m));
    m.type = type; m.group_id = gid;
    if (to)  strncpy(m.to_name, to, MAX_NAME_LEN - 1);
    if (txt) { strncpy(m.body, txt, MAX_BODY_LEN - 1); m.body_len = strlen(m.body); }
    fill_timestamp(m.timestamp, sizeof(m.timestamp));
    send_all(&m);
}

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : SERVER_PORT;
    g_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, host, &sa.sin_addr);
    if (connect(g_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { perror("connect"); return 1; }

    pthread_t t; pthread_create(&t, NULL, recv_th, NULL);

    char line[1024];
    while (fgets(line, sizeof(line), stdin)) {
        /* 把一行拆成: 命令 cmd + 第一参数 a + 其余 rest(可含空格, 直到行尾)。
         * "%899[^\n]" 表示读到换行前的所有字符。返回匹配数 <1 说明是空行, 跳过。 */
        char cmd[16] = {0}, a[64] = {0}, rest[900] = {0};
        int gid = 0;
        if (sscanf(line, "%15s %63s %899[^\n]", cmd, a, rest) < 1) continue;

        if      (!strcmp(cmd, "REG"))     cmd_login(1, a, rest);
        else if (!strcmp(cmd, "LOGIN"))   cmd_login(0, a, rest);
        else if (!strcmp(cmd, "PMSG"))    cmd_text(MSG_PRIVATE_CHAT, a, 0, rest);
        else if (!strcmp(cmd, "GMSG"))  { gid = atoi(a); cmd_text(MSG_GROUP_CHAT, NULL, gid, rest); }
        else if (!strcmp(cmd, "FADD"))    cmd_text(MSG_FRIEND_ADD, a, 0, NULL);
        else if (!strcmp(cmd, "FDEL"))    cmd_text(MSG_FRIEND_DEL, a, 0, NULL);
        else if (!strcmp(cmd, "BADD"))    cmd_text(MSG_BLACK_ADD, a, 0, NULL);
        else if (!strcmp(cmd, "BDEL"))    cmd_text(MSG_BLACK_DEL, a, 0, NULL);
        else if (!strcmp(cmd, "FLIST"))   cmd_text(MSG_FRIEND_LIST, NULL, 0, NULL);
        else if (!strcmp(cmd, "GCREATE")) cmd_text(MSG_GROUP_CREATE, NULL, 0, a);
        else if (!strcmp(cmd, "GJOIN")) { gid = atoi(a); cmd_text(MSG_GROUP_JOIN, NULL, gid, NULL); }
        else if (!strcmp(cmd, "GLIST"))   cmd_text(MSG_GROUP_LIST, NULL, 0, NULL);
        else if (!strcmp(cmd, "HISTP"))   cmd_text(MSG_HISTORY_PRIV, a, 0, NULL);
        else if (!strcmp(cmd, "HISTG")) { gid = atoi(a); cmd_text(MSG_HISTORY_GROUP, NULL, gid, NULL); }
        else if (!strcmp(cmd, "FREQ"))    cmd_text(MSG_FRIEND_REQ, a, 0, rest);
        else if (!strcmp(cmd, "FRACC"))   { /* reqid 在 a, 同意 */
            Message m; memset(&m,0,sizeof(m));
            m.type = MSG_FRIEND_REQ_REPLY; m.status = atoi(a); m.group_id = 1;
            send_all(&m);
        }
        else if (!strcmp(cmd, "FRREJ"))   {
            Message m; memset(&m,0,sizeof(m));
            m.type = MSG_FRIEND_REQ_REPLY; m.status = atoi(a); m.group_id = 0;
            send_all(&m);
        }
        else if (!strcmp(cmd, "FRLIST")) cmd_text(MSG_FRIEND_REQ_LIST, NULL, 0, NULL);
        else if (!strcmp(cmd, "GREQ"))   { gid = atoi(a); cmd_text(MSG_GROUP_JOIN_REQ, NULL, gid, rest); }
        else if (!strcmp(cmd, "GRACC"))  {
            Message m; memset(&m,0,sizeof(m));
            m.type = MSG_GROUP_JOIN_REPLY; m.status = atoi(a); m.group_id = 1;
            send_all(&m);
        }
        else if (!strcmp(cmd, "GRREJ"))  {
            Message m; memset(&m,0,sizeof(m));
            m.type = MSG_GROUP_JOIN_REPLY; m.status = atoi(a); m.group_id = 0;
            send_all(&m);
        }
        else if (!strcmp(cmd, "USEARCH")) cmd_text(MSG_USER_SEARCH, NULL, 0, a);
        else if (!strcmp(cmd, "GSEARCH")) cmd_text(MSG_GROUP_SEARCH, NULL, 0, a);
        else if (!strcmp(cmd, "QUIT"))    break;

        usleep(150000);   /* 给服务器/接收线程一点时间打印应答 */
    }
    close(g_fd);
    return 0;
}
