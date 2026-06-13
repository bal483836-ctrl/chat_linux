#ifndef CHAT_CLIENT_H
#define CHAT_CLIENT_H

#include "../common/protocol.h"
#include <gtk/gtk.h>

/* 全局客户端状态. 单进程单连接, 不需要太花哨的封装. */
typedef struct {
    int       sockfd;
    /* === 当前登录身份 === */
    char      account[MAX_NAME_LEN];   /* "100001" */
    char      nickname[MAX_NAME_LEN];
    int       avatar_color;
    pthread_t recv_tid;

    /* === 登录/注册窗 === */
    GtkWidget *login_win;
    GtkWidget *login_user;       /* 账号输入 */
    GtkWidget *login_pass;

    /* === 主窗 === */
    GtkWidget *main_win;
    GtkWidget *self_avatar;
    GtkWidget *self_nick_lbl;
    GtkWidget *self_acc_lbl;

    /* 三个左侧列表使用 GtkListBox 自绘行 */
    GtkWidget *friend_box;       /* GtkListBox */
    GtkWidget *group_box;
    GtkWidget *req_box;
    GtkWidget *req_count_lbl;

    GtkWidget *chat_header;
    GtkWidget *chat_avatar_area; /* 顶部对方头像区 */
    GtkWidget *add_btn;
    GtkTextBuffer *chat_buf;
    GtkWidget *input_entry;

    /* 当前聊天对象 */
    int       peer_is_group;
    char      peer_name[MAX_NAME_LEN];     /* 私聊: 对方账号; 群聊: 群名 */
    char      peer_nick[MAX_NAME_LEN];
    int       peer_color;
    int       peer_group_id;
} ClientCtx;

extern ClientCtx CTX;

/* 建立连接 / 关闭 */
int  net_connect(const char *host, int port);
void net_close(void);

/* 发送便捷函数 */
int  net_send(const Message *m);
int  net_send_text(int type, const char *to, int gid, const char *text);

/* 接收线程入口 */
void *recv_thread(void *arg);

/* UI 入口 */
void show_login(int argc, char **argv);
void show_main(void);
void ui_append_chat(const char *who, const char *time, const char *text);
void ui_refresh_friends(const char *body);
void ui_refresh_groups(const char *body);
void ui_refresh_requests(int kind, const char *body); /* kind: 0=好友申请,1=入群申请 */
void ui_add_request(int kind, int reqid, const char *acc, const char *nick, int color,
                    const char *gname, const char *hello, int gid);
void ui_notify_text(const char *title, const char *text);
void ui_search_result(int is_user, const char *body);

/* 绘制圆形头像: 调色板索引 0..9, 字符为昵称首字符 */
GtkWidget *avatar_widget(const char *nick, int color, int size);

#endif
