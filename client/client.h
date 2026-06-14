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

    /* 注册流程的临时态: 上传的头像字节 (PNG, 已缩到 64x64).
     * 用户点"确认注册"且服务器返回新账号后, 我们立刻把这段字节
     * 用 MSG_AVATAR_UPLOAD 发上去. 为 NULL 表示用户没选头像. */
    unsigned char *pending_avatar;
    unsigned long  pending_avatar_size;

    /* 头像缓存. key = 账号字符串, value = GdkPixbuf* (原始尺寸).
     * 任何 avatar_widget 在绘制时先查这张表; 若有图就画图, 否则回退
     * 到首字母圆形. 由 net.c 在收到 MSG_AVATAR_DATA 时填充. */
    GHashTable    *avatar_cache;
    /* 已经向服务器请求过头像的账号集合, 防止重复发请求.
     * (服务器即便没头像也会回 status=0 的 DATA, 缓存里存 NULL 标记) */
    GHashTable    *avatar_requested;

    /* account → GList<GtkDrawingArea*>. 每个 avatar_widget 在创建时
     * 把自身注册到这里, 销毁时摘掉. 收到新头像后, 我们对该账号下的
     * 所有 DrawingArea 调 gtk_widget_queue_draw 触发重绘. */
    GHashTable    *avatar_widgets;
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

/* 创建一个圆形头像控件.
 *   account  - 用于在缓存中查图片 (NULL 或 "" 表示不查, 直接画字母)
 *   nick     - 取首字符画在头像中央, 作为没有图片时的回退
 *   color    - 调色板索引 0..9
 *   size     - 边长 (px), 同时影响 widget min size 和 Cairo 圆半径
 * 返回值是 GtkDrawingArea, 调用方负责把它 pack 进自己的容器. */
GtkWidget *avatar_widget(const char *account, const char *nick, int color, int size);

/* 头像缓存维护. 由 net.c 在收到 MSG_AVATAR_DATA 时调用. */
void avatar_cache_put(const char *account, const unsigned char *png_bytes, int len);
/* 决定要不要给某账号发 MSG_AVATAR_GET. 已请求/已缓存的不会重复发. */
void avatar_request_if_needed(const char *account);

#endif
