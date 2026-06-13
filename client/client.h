#ifndef CHAT_CLIENT_H
#define CHAT_CLIENT_H

#include "../common/protocol.h"
#include <gtk/gtk.h>

/* 全局客户端状态. 单进程单连接, 不需要太花哨的封装. */
typedef struct {
    int       sockfd;
    char      username[MAX_NAME_LEN];
    pthread_t recv_tid;

    /* === GTK 控件 === */
    GtkWidget *login_win;
    GtkWidget *login_user;
    GtkWidget *login_pass;
    GtkWidget *login_host;

    GtkWidget *main_win;
    GtkWidget *friend_view;     /* GtkTreeView */
    GtkListStore *friend_store; /* name, online(bool), black(bool) */
    GtkWidget *group_view;
    GtkListStore *group_store;  /* gid, name */
    GtkWidget *chat_header;
    GtkWidget *add_btn;         /* 右上角 "+" 按钮 */
    GtkTextBuffer *chat_buf;
    GtkWidget *input_entry;

    /* 通知/申请 (好友申请 + 群入群申请合一显示) */
    GtkWidget    *req_view;
    GtkListStore *req_store;    /* kind(int) reqid(int) text(str) hello(str) */
    GtkWidget    *req_count_lbl;/* 用于 tab label 上的小红点数字 */

    /* 当前聊天对象 */
    int       peer_is_group;   /* 0=私聊 1=群聊 */
    char      peer_name[MAX_NAME_LEN];
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
void ui_add_request(int kind, int reqid, const char *who, const char *gname, const char *hello, int gid);
void ui_notify_text(const char *title, const char *text);
void ui_search_result(int is_user, const char *body);

#endif
