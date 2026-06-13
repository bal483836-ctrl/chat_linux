/* =========================================================
 *  chat_linux 客户端 GTK3 界面 (QQ 风格)
 *
 *  布局:
 *    登录:    [注册(小)]  [    登录(大)    ]
 *    注册:    [返回(小)]  [   确认注册(大) ]
 *
 *    主窗:
 *     ┌─好友/群/通知─┐ ┌─当前会话           [ + ]┐
 *     │ alice ●     │ │  聊天历史                 │
 *     │ bob          │ │                           │
 *     │ ...          │ │                           │
 *     │              │ │ [输入框        ] [文件][发送]
 *     └──────────────┘ └───────────────────────────┘
 *
 *   操作交互:
 *     右键好友 → 私聊 / 历史 / 删除 / 拉黑·解除
 *     右键群组 → 进入会话 / 历史
 *     右上"+" → 添加好友 / 添加群 / 创建群
 *     通知 tab → 待处理好友/入群申请, 行内"同意/拒绝"
 * ========================================================= */
#include "client.h"
#include "../common/net_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---- 前向声明 ---- */
static void do_login_clicked(GtkButton *, gpointer);
static void open_register_win(GtkButton *, gpointer);
static void do_register_confirm(GtkButton *, gpointer);
static void on_friend_selected(GtkTreeSelection *, gpointer);
static void on_group_selected (GtkTreeSelection *, gpointer);
static void on_send_clicked   (GtkButton *, gpointer);
static void on_sendfile       (GtkButton *, gpointer);
static gboolean on_friend_button_press(GtkWidget *, GdkEventButton *, gpointer);
static gboolean on_group_button_press (GtkWidget *, GdkEventButton *, gpointer);
static void on_add_clicked    (GtkButton *, gpointer);
static void open_search_dialog(int is_user);   /* 1=用户 0=群 */
static void open_create_group_dialog(void);

/* ---- 工具 ---- */
static const char *server_host(void) {
    const char *h = getenv("CHAT_SERVER_HOST");
    return (h && *h) ? h : "127.0.0.1";
}

static gboolean on_login_close(GtkWidget *w, GdkEvent *e, gpointer ud) {
    (void)w; (void)e; (void)ud;
    if (!CTX.main_win) gtk_main_quit();
    return FALSE;
}

/* 弹一个简单消息框 */
static void msgbox(GtkWindow *parent, GtkMessageType type, const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s", buf);
    gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d);
}

/* 让用户输入一段文本; ok=1 才填充 out */
static int prompt_text(const char *title, const char *prompt, char *out, int outsz) {
    GtkWidget *d = gtk_dialog_new_with_buttons(title, GTK_WINDOW(CTX.main_win),
        GTK_DIALOG_MODAL,
        "取消", GTK_RESPONSE_CANCEL,
        "确定", GTK_RESPONSE_OK, NULL);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(d));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *lbl = gtk_label_new(prompt);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    GtkWidget *ent = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(ent), TRUE);
    gtk_widget_set_size_request(ent, 260, -1);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ent, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(area), box);
    gtk_widget_set_can_default(gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_OK), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    gtk_widget_show_all(d);
    int rc = gtk_dialog_run(GTK_DIALOG(d));
    int ok = 0;
    if (rc == GTK_RESPONSE_OK) {
        strncpy(out, gtk_entry_get_text(GTK_ENTRY(ent)), outsz - 1);
        out[outsz - 1] = 0;
        ok = (*out != 0);
    }
    gtk_widget_destroy(d);
    return ok;
}

/* ===================== 登录窗 ===================== */
void show_login(int argc, char **argv) {
    gtk_init(&argc, &argv);

    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w), "chat_linux 登录");
    gtk_window_set_default_size(GTK_WINDOW(w), 340, 220);
    gtk_window_set_position(GTK_WINDOW(w), GTK_WIN_POS_CENTER);
    gtk_window_set_resizable(GTK_WINDOW(w), FALSE);
    g_signal_connect(w, "delete-event", G_CALLBACK(on_login_close), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 18);
    gtk_container_add(GTK_CONTAINER(w), vbox);

    /* logo / 标题 */
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title),
        "<span size='x-large' weight='bold'>chat_linux</span>");
    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 4);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);

    GtkWidget *lu = gtk_label_new("用户名");
    GtkWidget *lp = gtk_label_new("密码");
    GtkWidget *eu = gtk_entry_new();
    GtkWidget *ep = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(eu), "请输入用户名");
    gtk_entry_set_placeholder_text(GTK_ENTRY(ep), "请输入密码");
    gtk_entry_set_visibility(GTK_ENTRY(ep), FALSE);
    gtk_widget_set_hexpand(eu, TRUE);
    gtk_widget_set_hexpand(ep, TRUE);
    gtk_grid_attach(GTK_GRID(grid), lu, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), eu, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), lp, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ep, 1, 1, 1, 1);

    /* 按钮行: [注册(小)]  [           登录(大)           ] */
    GtkWidget *btnbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *br = gtk_button_new_with_label("注册");
    GtkWidget *bl = gtk_button_new_with_label("登 录");
    gtk_widget_set_size_request(br, 80,  36);
    gtk_widget_set_size_request(bl, 200, 40);
    /* 给登录按钮加点视觉重量 */
    GtkStyleContext *sc = gtk_widget_get_style_context(bl);
    gtk_style_context_add_class(sc, "suggested-action");
    gtk_box_pack_start(GTK_BOX(btnbox), br, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btnbox), bl, TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(vbox), btnbox, FALSE, FALSE, 0);

    CTX.login_win  = w;
    CTX.login_user = eu;
    CTX.login_pass = ep;
    CTX.login_host = NULL;

    g_signal_connect(bl, "clicked",  G_CALLBACK(do_login_clicked),   NULL);
    g_signal_connect(br, "clicked",  G_CALLBACK(open_register_win),  NULL);
    g_signal_connect(eu, "activate", G_CALLBACK(do_login_clicked),   NULL);
    g_signal_connect(ep, "activate", G_CALLBACK(do_login_clicked),   NULL);

    gtk_widget_show_all(w);
    gtk_main();
}

static void do_login_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    const char *u = gtk_entry_get_text(GTK_ENTRY(CTX.login_user));
    const char *p = gtk_entry_get_text(GTK_ENTRY(CTX.login_pass));
    if (!*u || !*p) { msgbox(GTK_WINDOW(CTX.login_win), GTK_MESSAGE_WARNING, "请填写用户名和密码"); return; }
    if (CTX.sockfd <= 0 && net_connect(server_host(), SERVER_PORT) < 0) {
        msgbox(GTK_WINDOW(CTX.login_win), GTK_MESSAGE_ERROR,
            "无法连接服务器 %s:%d\n请确认 chat_server 已启动", server_host(), SERVER_PORT);
        return;
    }
    char body[128]; snprintf(body, sizeof(body), "%s\n%s", u, p);
    Message m; memset(&m, 0, sizeof(m));
    m.type = MSG_LOGIN;
    strncpy(m.body, body, MAX_BODY_LEN - 1); m.body_len = strlen(body);
    net_send(&m);
    Message resp;
    if (recv_msg(CTX.sockfd, &resp) != 0 || resp.type != MSG_RESPONSE || resp.status != RS_OK) {
        msgbox(GTK_WINDOW(CTX.login_win), GTK_MESSAGE_ERROR,
            "登录失败: %s", resp.body[0] ? resp.body : "用户名或密码错误");
        net_close();
        return;
    }
    strncpy(CTX.username, u, MAX_NAME_LEN - 1);
    show_main();
    gtk_widget_hide(CTX.login_win);
    pthread_create(&CTX.recv_tid, NULL, recv_thread, NULL);
}

/* ===================== 注册窗 ===================== */
static GtkWidget *reg_win = NULL;
static GtkWidget *reg_user = NULL;
static GtkWidget *reg_pass = NULL;
static GtkWidget *reg_pass2 = NULL;

static void open_register_win(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    if (reg_win) { gtk_window_present(GTK_WINDOW(reg_win)); return; }
    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w), "chat_linux 注册");
    gtk_window_set_default_size(GTK_WINDOW(w), 360, 260);
    gtk_window_set_position(GTK_WINDOW(w), GTK_WIN_POS_CENTER);
    gtk_window_set_transient_for(GTK_WINDOW(w), GTK_WINDOW(CTX.login_win));
    gtk_window_set_modal(GTK_WINDOW(w), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(w), FALSE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 18);
    gtk_container_add(GTK_CONTAINER(w), vbox);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title),
        "<span size='large' weight='bold'>欢迎注册</span>");
    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 4);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);

    GtkWidget *lu = gtk_label_new("用户名");
    GtkWidget *lp = gtk_label_new("密码");
    GtkWidget *l2 = gtk_label_new("确认密码");
    reg_user = gtk_entry_new(); reg_pass = gtk_entry_new(); reg_pass2 = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(reg_user), "字母/数字, 不少于 1 位");
    gtk_entry_set_placeholder_text(GTK_ENTRY(reg_pass), "请输入密码");
    gtk_entry_set_placeholder_text(GTK_ENTRY(reg_pass2),"再输入一次密码");
    gtk_entry_set_visibility(GTK_ENTRY(reg_pass),  FALSE);
    gtk_entry_set_visibility(GTK_ENTRY(reg_pass2), FALSE);
    gtk_widget_set_hexpand(reg_user, TRUE);
    gtk_grid_attach(GTK_GRID(grid), lu,        0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), reg_user,  1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), lp,        0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), reg_pass,  1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), l2,        0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), reg_pass2, 1, 2, 1, 1);

    /* 按钮行: [返回登录(小)]  [   确认注册(大)   ] */
    GtkWidget *btnbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *bcan = gtk_button_new_with_label("返回");
    GtkWidget *bok  = gtk_button_new_with_label("确认注册");
    gtk_widget_set_size_request(bcan, 80,  36);
    gtk_widget_set_size_request(bok,  220, 40);
    gtk_style_context_add_class(gtk_widget_get_style_context(bok), "suggested-action");
    gtk_box_pack_start(GTK_BOX(btnbox), bcan, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btnbox), bok,  TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(vbox), btnbox, FALSE, FALSE, 0);

    g_signal_connect(bok,  "clicked", G_CALLBACK(do_register_confirm), NULL);
    g_signal_connect_swapped(bcan, "clicked", G_CALLBACK(gtk_widget_destroy), w);
    g_signal_connect(w, "destroy", G_CALLBACK(gtk_widget_destroyed), &reg_win);

    reg_win = w;
    gtk_widget_show_all(w);
}

static void do_register_confirm(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    const char *u  = gtk_entry_get_text(GTK_ENTRY(reg_user));
    const char *p  = gtk_entry_get_text(GTK_ENTRY(reg_pass));
    const char *p2 = gtk_entry_get_text(GTK_ENTRY(reg_pass2));
    if (!*u || !*p)        { msgbox(GTK_WINDOW(reg_win), GTK_MESSAGE_WARNING, "用户名和密码不能为空"); return; }
    if (strcmp(p, p2) != 0){ msgbox(GTK_WINDOW(reg_win), GTK_MESSAGE_ERROR,   "两次密码输入不一致");   return; }
    if (CTX.sockfd <= 0 && net_connect(server_host(), SERVER_PORT) < 0) {
        msgbox(GTK_WINDOW(reg_win), GTK_MESSAGE_ERROR, "无法连接服务器"); return;
    }
    char body[128]; snprintf(body, sizeof(body), "%s\n%s", u, p);
    Message m; memset(&m, 0, sizeof(m));
    m.type = MSG_REGISTER;
    strncpy(m.body, body, MAX_BODY_LEN - 1); m.body_len = strlen(body);
    net_send(&m);
    Message resp;
    if (recv_msg(CTX.sockfd, &resp) != 0) return;
    int ok = (resp.type == MSG_RESPONSE && resp.status == RS_OK);
    msgbox(GTK_WINDOW(reg_win),
           ok ? GTK_MESSAGE_INFO : GTK_MESSAGE_ERROR,
           "%s", ok ? "注册成功, 可以返回登录" : "注册失败: 用户已存在或服务器拒绝");
    if (ok) {
        gtk_entry_set_text(GTK_ENTRY(CTX.login_user), u);
        gtk_entry_set_text(GTK_ENTRY(CTX.login_pass), "");
        gtk_widget_grab_focus(CTX.login_pass);
        gtk_widget_destroy(reg_win);
    }
}

/* ===================== 主窗 ===================== */
void show_main(void) {
    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    char title[128];
    snprintf(title, sizeof(title), "chat_linux - %s", CTX.username);
    gtk_window_set_title(GTK_WINDOW(w), title);
    gtk_window_set_default_size(GTK_WINDOW(w), 880, 580);
    gtk_window_set_position(GTK_WINDOW(w), GTK_WIN_POS_CENTER);
    g_signal_connect(w, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    CTX.main_win = w;

    GtkWidget *hpane = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_container_add(GTK_CONTAINER(w), hpane);

    /* ========== 左侧: notebook 三页 ========== */
    GtkWidget *nb = gtk_notebook_new();

    /* 好友 */
    CTX.friend_store = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);
    CTX.friend_view  = gtk_tree_view_new_with_model(GTK_TREE_MODEL(CTX.friend_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(CTX.friend_view), TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(CTX.friend_view),
        gtk_tree_view_column_new_with_attributes("好友", gtk_cell_renderer_text_new(),  "text",   0, NULL));
    gtk_tree_view_append_column(GTK_TREE_VIEW(CTX.friend_view),
        gtk_tree_view_column_new_with_attributes("在线", gtk_cell_renderer_toggle_new(), "active", 1, NULL));
    gtk_tree_view_append_column(GTK_TREE_VIEW(CTX.friend_view),
        gtk_tree_view_column_new_with_attributes("黑名单", gtk_cell_renderer_toggle_new(), "active", 2, NULL));
    GtkWidget *fs = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(fs), CTX.friend_view);
    gtk_widget_set_size_request(fs, 240, 320);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(CTX.friend_view)),
                     "changed", G_CALLBACK(on_friend_selected), NULL);
    g_signal_connect(CTX.friend_view, "button-press-event",
                     G_CALLBACK(on_friend_button_press), NULL);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), fs, gtk_label_new("好友"));

    /* 群组 */
    CTX.group_store = gtk_list_store_new(2, G_TYPE_INT, G_TYPE_STRING);
    CTX.group_view  = gtk_tree_view_new_with_model(GTK_TREE_MODEL(CTX.group_store));
    gtk_tree_view_append_column(GTK_TREE_VIEW(CTX.group_view),
        gtk_tree_view_column_new_with_attributes("群号", gtk_cell_renderer_text_new(), "text", 0, NULL));
    gtk_tree_view_append_column(GTK_TREE_VIEW(CTX.group_view),
        gtk_tree_view_column_new_with_attributes("群名", gtk_cell_renderer_text_new(), "text", 1, NULL));
    GtkWidget *gs = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(gs), CTX.group_view);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(CTX.group_view)),
                     "changed", G_CALLBACK(on_group_selected), NULL);
    g_signal_connect(CTX.group_view, "button-press-event",
                     G_CALLBACK(on_group_button_press), NULL);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), gs, gtk_label_new("群组"));

    /* 通知 (好友申请 + 入群申请). 列: 描述, [同意], [拒绝] */
    CTX.req_store = gtk_list_store_new(4,
        G_TYPE_INT,    /* kind 0=好友 1=入群 */
        G_TYPE_INT,    /* reqid */
        G_TYPE_STRING, /* 描述 */
        G_TYPE_INT);   /* gid (仅入群用) */
    CTX.req_view  = gtk_tree_view_new_with_model(GTK_TREE_MODEL(CTX.req_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(CTX.req_view), TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(CTX.req_view),
        gtk_tree_view_column_new_with_attributes("申请", gtk_cell_renderer_text_new(), "text", 2, NULL));
    GtkWidget *rs = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(rs), CTX.req_view);
    g_signal_connect(CTX.req_view, "button-press-event",
                     G_CALLBACK(on_friend_button_press), GINT_TO_POINTER(99)); /* 复用入口, 走 req 分支 */
    GtkWidget *tab_lbl = gtk_label_new("通知");
    CTX.req_count_lbl = tab_lbl;
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), rs, tab_lbl);

    gtk_paned_add1(GTK_PANED(hpane), nb);

    /* ========== 右侧: 聊天面板 ========== */
    GtkWidget *rvbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(rvbox), 6);

    /* 顶部: 标题 + "+" 按钮 */
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    CTX.chat_header = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(CTX.chat_header),
        "<span color='#888'>请在左侧选择好友或群开始聊天</span>");
    gtk_label_set_xalign(GTK_LABEL(CTX.chat_header), 0.0);
    gtk_widget_set_hexpand(CTX.chat_header, TRUE);
    CTX.add_btn = gtk_button_new_from_icon_name("list-add", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(CTX.add_btn, "添加好友/群");
    gtk_box_pack_start(GTK_BOX(hdr), CTX.chat_header, TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(hdr), CTX.add_btn,    FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rvbox), hdr, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rvbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    /* 聊天显示区 */
    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
    CTX.chat_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    GtkWidget *sv = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sv), tv);
    gtk_box_pack_start(GTK_BOX(rvbox), sv, TRUE, TRUE, 0);

    /* 输入行 */
    GtkWidget *ibox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    CTX.input_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(CTX.input_entry), "输入消息, 回车发送");
    GtkWidget *bfile = gtk_button_new_from_icon_name("mail-attachment", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(bfile, "传文件 (仅私聊)");
    GtkWidget *bsnd  = gtk_button_new_with_label("发送");
    gtk_widget_set_size_request(bsnd, 80, -1);
    gtk_box_pack_start(GTK_BOX(ibox), CTX.input_entry, TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(ibox), bfile,           FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ibox), bsnd,            FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rvbox), ibox, FALSE, FALSE, 0);

    g_signal_connect(bsnd,            "clicked",  G_CALLBACK(on_send_clicked), NULL);
    g_signal_connect(CTX.input_entry, "activate", G_CALLBACK(on_send_clicked), NULL);
    g_signal_connect(bfile,           "clicked",  G_CALLBACK(on_sendfile),     NULL);
    g_signal_connect(CTX.add_btn,     "clicked",  G_CALLBACK(on_add_clicked),  NULL);

    gtk_paned_add2(GTK_PANED(hpane), rvbox);
    gtk_paned_set_position(GTK_PANED(hpane), 270);

    gtk_widget_show_all(w);

    /* 兜底拉一次列表 */
    Message m;
    memset(&m, 0, sizeof(m)); m.type = MSG_FRIEND_LIST;       net_send(&m);
    memset(&m, 0, sizeof(m)); m.type = MSG_GROUP_LIST;        net_send(&m);
    memset(&m, 0, sizeof(m)); m.type = MSG_FRIEND_REQ_LIST;   net_send(&m);
    memset(&m, 0, sizeof(m)); m.type = MSG_GROUP_JOIN_REQ_LIST; net_send(&m);
}

/* ===================== 选中行 → 打开会话 ===================== */
static void on_friend_selected(GtkTreeSelection *sel, gpointer ud) {
    (void)ud;
    GtkTreeModel *m; GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &m, &it)) return;
    gchar *name = NULL;
    gtk_tree_model_get(m, &it, 0, &name, -1);
    if (!name) return;
    CTX.peer_is_group = 0;
    strncpy(CTX.peer_name, name, MAX_NAME_LEN - 1);
    CTX.peer_group_id  = 0;
    char hdr[256];
    snprintf(hdr, sizeof(hdr), "<b>与 %s 私聊</b>", name);
    gtk_label_set_markup(GTK_LABEL(CTX.chat_header), hdr);
    gtk_text_buffer_set_text(CTX.chat_buf, "", -1);
    Message hm; memset(&hm, 0, sizeof(hm));
    hm.type = MSG_HISTORY_PRIV;
    strncpy(hm.to_name, name, MAX_NAME_LEN - 1);
    net_send(&hm);
    g_free(name);
}

static void on_group_selected(GtkTreeSelection *sel, gpointer ud) {
    (void)ud;
    GtkTreeModel *m; GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &m, &it)) return;
    gint gid = 0; gchar *name = NULL;
    gtk_tree_model_get(m, &it, 0, &gid, 1, &name, -1);
    CTX.peer_is_group = 1;
    CTX.peer_group_id = gid;
    if (name) strncpy(CTX.peer_name, name, MAX_NAME_LEN - 1);
    char hdr[256];
    snprintf(hdr, sizeof(hdr), "<b>群聊: %s</b>  <span color='#888'>#%d</span>", name?name:"", gid);
    gtk_label_set_markup(GTK_LABEL(CTX.chat_header), hdr);
    gtk_text_buffer_set_text(CTX.chat_buf, "", -1);
    Message hm; memset(&hm, 0, sizeof(hm));
    hm.type = MSG_HISTORY_GROUP; hm.group_id = gid;
    net_send(&hm);
    if (name) g_free(name);
}

/* ===================== 发送 ===================== */
static void on_send_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    const char *text = gtk_entry_get_text(GTK_ENTRY(CTX.input_entry));
    if (!*text) return;
    if (CTX.peer_is_group && CTX.peer_group_id > 0) {
        net_send_text(MSG_GROUP_CHAT, NULL, CTX.peer_group_id, text);
    } else if (!CTX.peer_is_group && *CTX.peer_name) {
        net_send_text(MSG_PRIVATE_CHAT, CTX.peer_name, 0, text);
    } else {
        msgbox(GTK_WINDOW(CTX.main_win), GTK_MESSAGE_WARNING, "请先在左侧选择会话对象");
        return;
    }
    char ts[32]; fill_timestamp(ts, sizeof(ts));
    char me[64]; snprintf(me, sizeof(me), "%s (我)", CTX.username);
    ui_append_chat(me, ts, text);
    gtk_entry_set_text(GTK_ENTRY(CTX.input_entry), "");
}

/* ===================== 文件 (复用原逻辑) ===================== */
static void on_sendfile(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    if (CTX.peer_is_group || !*CTX.peer_name) {
        msgbox(GTK_WINDOW(CTX.main_win), GTK_MESSAGE_WARNING, "请先选择一位好友以传文件"); return;
    }
    GtkWidget *fc = gtk_file_chooser_dialog_new("选择文件", GTK_WINDOW(CTX.main_win),
        GTK_FILE_CHOOSER_ACTION_OPEN, "取消", GTK_RESPONSE_CANCEL,
        "打开", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(fc)) != GTK_RESPONSE_ACCEPT) { gtk_widget_destroy(fc); return; }
    char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
    gtk_widget_destroy(fc);
    if (!path) return;
    FILE *fp = fopen(path, "rb");
    if (!fp) { g_free(path); return; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    const char *fname = strrchr(path, '/'); fname = fname ? fname + 1 : path;
    Message m; memset(&m, 0, sizeof(m));
    m.type = MSG_FILE_BEGIN; m.status = (uint32_t)sz;
    strncpy(m.to_name, CTX.peer_name, MAX_NAME_LEN - 1);
    strncpy(m.body, fname, MAX_BODY_LEN - 1); m.body_len = strlen(fname);
    fill_timestamp(m.timestamp, sizeof(m.timestamp));
    net_send(&m);
    while (1) {
        memset(&m, 0, sizeof(m));
        m.type = MSG_FILE_CHUNK;
        strncpy(m.to_name, CTX.peer_name, MAX_NAME_LEN - 1);
        size_t n = fread(m.body, 1, FILE_CHUNK_SIZE, fp);
        if (n == 0) break;
        m.body_len = (uint32_t)n;
        net_send(&m);
    }
    memset(&m, 0, sizeof(m)); m.type = MSG_FILE_END;
    strncpy(m.to_name, CTX.peer_name, MAX_NAME_LEN - 1);
    net_send(&m);
    fclose(fp);
    char ts[32]; fill_timestamp(ts, sizeof(ts));
    char log[256]; snprintf(log, sizeof(log), "已发送文件 %s (%ld 字节)", fname, sz);
    ui_append_chat("[文件]", ts, log);
    g_free(path);
}

/* ===================== 好友右键菜单 ===================== */
typedef struct { char name[MAX_NAME_LEN]; int is_black; } FriendCtx;

static void menu_chat(GtkMenuItem *mi, gpointer ud) {
    FriendCtx *fc = ud;
    /* 切换到该好友: 模拟在 friend_view 上选中 */
    GtkTreeModel *m = GTK_TREE_MODEL(CTX.friend_store);
    GtkTreeIter it;
    if (gtk_tree_model_get_iter_first(m, &it)) {
        do {
            gchar *nm = NULL;
            gtk_tree_model_get(m, &it, 0, &nm, -1);
            if (nm && strcmp(nm, fc->name) == 0) {
                gtk_tree_selection_select_iter(
                    gtk_tree_view_get_selection(GTK_TREE_VIEW(CTX.friend_view)), &it);
                g_free(nm); break;
            }
            if (nm) g_free(nm);
        } while (gtk_tree_model_iter_next(m, &it));
    }
}
static void menu_history(GtkMenuItem *mi, gpointer ud) {
    FriendCtx *fc = ud;
    gtk_text_buffer_set_text(CTX.chat_buf, "", -1);
    net_send_text(MSG_HISTORY_PRIV, fc->name, 0, NULL);
}
static void menu_del(GtkMenuItem *mi, gpointer ud) {
    FriendCtx *fc = ud;
    net_send_text(MSG_FRIEND_DEL, fc->name, 0, NULL);
    net_send_text(MSG_FRIEND_LIST, NULL, 0, NULL);
}
static void menu_black_toggle(GtkMenuItem *mi, gpointer ud) {
    FriendCtx *fc = ud;
    net_send_text(fc->is_black ? MSG_BLACK_DEL : MSG_BLACK_ADD, fc->name, 0, NULL);
    net_send_text(MSG_FRIEND_LIST, NULL, 0, NULL);
}
static void free_friend_ctx(GtkWidget *w, gpointer ud) { (void)w; g_free(ud); }

static void popup_friend_menu(GdkEventButton *ev, const char *name, int is_black) {
    FriendCtx *fc = g_malloc0(sizeof(*fc));
    strncpy(fc->name, name, MAX_NAME_LEN - 1);
    fc->is_black = is_black;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mi_chat = gtk_menu_item_new_with_label("发起会话");
    GtkWidget *mi_his  = gtk_menu_item_new_with_label("查看历史");
    GtkWidget *mi_blk  = gtk_menu_item_new_with_label(is_black ? "移出黑名单" : "加入黑名单");
    GtkWidget *mi_del  = gtk_menu_item_new_with_label("删除好友");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_chat);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_his);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_blk);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_del);
    g_signal_connect(mi_chat,"activate", G_CALLBACK(menu_chat),         fc);
    g_signal_connect(mi_his, "activate", G_CALLBACK(menu_history),      fc);
    g_signal_connect(mi_blk, "activate", G_CALLBACK(menu_black_toggle), fc);
    g_signal_connect(mi_del, "activate", G_CALLBACK(menu_del),          fc);
    g_signal_connect(menu, "destroy", G_CALLBACK(free_friend_ctx), fc);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

/* ===================== 群组右键菜单 ===================== */
typedef struct { int gid; char name[MAX_NAME_LEN]; } GroupCtx;

static void gmenu_chat(GtkMenuItem *mi, gpointer ud) {
    GroupCtx *gc = ud;
    GtkTreeModel *m = GTK_TREE_MODEL(CTX.group_store);
    GtkTreeIter it;
    if (gtk_tree_model_get_iter_first(m, &it)) {
        do {
            gint gid = 0;
            gtk_tree_model_get(m, &it, 0, &gid, -1);
            if (gid == gc->gid) {
                gtk_tree_selection_select_iter(
                    gtk_tree_view_get_selection(GTK_TREE_VIEW(CTX.group_view)), &it);
                break;
            }
        } while (gtk_tree_model_iter_next(m, &it));
    }
}
static void gmenu_history(GtkMenuItem *mi, gpointer ud) {
    GroupCtx *gc = ud;
    gtk_text_buffer_set_text(CTX.chat_buf, "", -1);
    Message hm; memset(&hm, 0, sizeof(hm));
    hm.type = MSG_HISTORY_GROUP; hm.group_id = gc->gid;
    net_send(&hm);
}
static void free_group_ctx(GtkWidget *w, gpointer ud) { (void)w; g_free(ud); }

static void popup_group_menu(GdkEventButton *ev, int gid, const char *name) {
    GroupCtx *gc = g_malloc0(sizeof(*gc));
    gc->gid = gid;
    if (name) strncpy(gc->name, name, MAX_NAME_LEN - 1);
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mi_chat = gtk_menu_item_new_with_label("发起会话");
    GtkWidget *mi_his  = gtk_menu_item_new_with_label("查看历史");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_chat);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_his);
    g_signal_connect(mi_chat, "activate", G_CALLBACK(gmenu_chat),    gc);
    g_signal_connect(mi_his,  "activate", G_CALLBACK(gmenu_history), gc);
    g_signal_connect(menu, "destroy", G_CALLBACK(free_group_ctx), gc);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

/* ===================== 申请项菜单: 同意/拒绝 ===================== */
typedef struct { int kind; int reqid; int gid; char who[64]; } ReqCtx;

static void rmenu_accept(GtkMenuItem *mi, gpointer ud) {
    ReqCtx *rc = ud;
    Message m; memset(&m, 0, sizeof(m));
    m.type     = rc->kind == 0 ? MSG_FRIEND_REQ_REPLY : MSG_GROUP_JOIN_REPLY;
    m.status   = rc->reqid;
    m.group_id = 1;
    net_send(&m);
    /* 刷新通知和好友/群列表 */
    Message q; memset(&q, 0, sizeof(q));
    q.type = (rc->kind == 0) ? MSG_FRIEND_REQ_LIST : MSG_GROUP_JOIN_REQ_LIST; net_send(&q);
    memset(&q, 0, sizeof(q)); q.type = MSG_FRIEND_LIST; net_send(&q);
    memset(&q, 0, sizeof(q)); q.type = MSG_GROUP_LIST;  net_send(&q);
}
static void rmenu_reject(GtkMenuItem *mi, gpointer ud) {
    ReqCtx *rc = ud;
    Message m; memset(&m, 0, sizeof(m));
    m.type     = rc->kind == 0 ? MSG_FRIEND_REQ_REPLY : MSG_GROUP_JOIN_REPLY;
    m.status   = rc->reqid;
    m.group_id = 0;
    net_send(&m);
    Message q; memset(&q, 0, sizeof(q));
    q.type = (rc->kind == 0) ? MSG_FRIEND_REQ_LIST : MSG_GROUP_JOIN_REQ_LIST; net_send(&q);
}
static void free_req_ctx(GtkWidget *w, gpointer ud) { (void)w; g_free(ud); }

static void popup_req_menu(GdkEventButton *ev, int kind, int reqid, int gid) {
    ReqCtx *rc = g_malloc0(sizeof(*rc));
    rc->kind = kind; rc->reqid = reqid; rc->gid = gid;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mi_y = gtk_menu_item_new_with_label("同意");
    GtkWidget *mi_n = gtk_menu_item_new_with_label("拒绝");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_y);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_n);
    g_signal_connect(mi_y, "activate", G_CALLBACK(rmenu_accept), rc);
    g_signal_connect(mi_n, "activate", G_CALLBACK(rmenu_reject), rc);
    g_signal_connect(menu, "destroy", G_CALLBACK(free_req_ctx), rc);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

/* ===================== button-press 路由 ===================== */
/* ud == GINT_TO_POINTER(99) 表示是申请列表 */
static gboolean on_friend_button_press(GtkWidget *view, GdkEventButton *ev, gpointer ud) {
    if (ev->type != GDK_BUTTON_PRESS) return FALSE;
    if (ev->button != 3 && !(ev->button == 1 && GPOINTER_TO_INT(ud) == 99)) return FALSE;
    /* 选中被点的行 */
    GtkTreePath *path = NULL;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view),
            ev->x, ev->y, &path, NULL, NULL, NULL)) return FALSE;
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(view), path, NULL, FALSE);
    GtkTreeModel *m = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    GtkTreeIter it;
    gtk_tree_model_get_iter(m, &it, path);
    gtk_tree_path_free(path);
    if (GPOINTER_TO_INT(ud) == 99) {
        /* 申请列表行: 取 kind/reqid/gid */
        if (ev->button != 1 && ev->button != 3) return FALSE;
        gint kind = 0, reqid = 0, gid = 0;
        gtk_tree_model_get(m, &it, 0, &kind, 1, &reqid, 3, &gid, -1);
        popup_req_menu(ev, kind, reqid, gid);
        return TRUE;
    }
    /* 好友列表 */
    gchar *name = NULL; gboolean black = FALSE;
    gtk_tree_model_get(m, &it, 0, &name, 2, &black, -1);
    if (name) {
        popup_friend_menu(ev, name, black ? 1 : 0);
        g_free(name);
    }
    return TRUE;
}

static gboolean on_group_button_press(GtkWidget *view, GdkEventButton *ev, gpointer ud) {
    (void)ud;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3) return FALSE;
    GtkTreePath *path = NULL;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view),
            ev->x, ev->y, &path, NULL, NULL, NULL)) return FALSE;
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(view), path, NULL, FALSE);
    GtkTreeModel *m = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    GtkTreeIter it;
    gtk_tree_model_get_iter(m, &it, path);
    gtk_tree_path_free(path);
    gint gid = 0; gchar *name = NULL;
    gtk_tree_model_get(m, &it, 0, &gid, 1, &name, -1);
    popup_group_menu(ev, gid, name);
    if (name) g_free(name);
    return TRUE;
}

/* ===================== 右上 "+" 菜单 ===================== */
static void plus_addfriend(GtkMenuItem *mi, gpointer ud) { (void)mi;(void)ud; open_search_dialog(1); }
static void plus_addgroup (GtkMenuItem *mi, gpointer ud) { (void)mi;(void)ud; open_search_dialog(0); }
static void plus_creategroup(GtkMenuItem *mi, gpointer ud) { (void)mi;(void)ud; open_create_group_dialog(); }

static void on_add_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mi_f = gtk_menu_item_new_with_label("添加好友");
    GtkWidget *mi_g = gtk_menu_item_new_with_label("添加群");
    GtkWidget *mi_c = gtk_menu_item_new_with_label("创建群");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_f);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_g);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi_c);
    g_signal_connect(mi_f, "activate", G_CALLBACK(plus_addfriend),   NULL);
    g_signal_connect(mi_g, "activate", G_CALLBACK(plus_addgroup),    NULL);
    g_signal_connect(mi_c, "activate", G_CALLBACK(plus_creategroup), NULL);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_widget(GTK_MENU(menu), GTK_WIDGET(b),
        GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
}

/* ===================== 搜索对话框 ===================== */
typedef struct {
    int           is_user;
    GtkWidget    *dlg;
    GtkWidget    *entry;
    GtkWidget    *view;
    GtkListStore *store;
} SearchDlg;

static SearchDlg *g_search_dlg = NULL;   /* 当前打开的搜索框 (单例) */

void ui_search_result(int is_user, const char *body) {
    if (!g_search_dlg || g_search_dlg->is_user != is_user) return;
    gtk_list_store_clear(g_search_dlg->store);
    if (!body || !*body) return;
    char *dup = g_strdup(body);
    char *save = NULL;
    char *line = strtok_r(dup, "\n", &save);
    while (line) {
        if (is_user) {
            /* "name\tonline" */
            char *t = strchr(line, '\t');
            int online = t ? atoi(t + 1) : 0;
            if (t) *t = 0;
            GtkTreeIter it;
            gtk_list_store_append(g_search_dlg->store, &it);
            gtk_list_store_set(g_search_dlg->store, &it, 0, 0, 1, line,
                               2, online ? "在线" : "离线", -1);
        } else {
            /* "gid\tname\towner\tcnt" */
            char *t1 = strchr(line, '\t');     if (!t1) goto next;
            *t1 = 0; int gid = atoi(line);
            char *t2 = strchr(t1+1, '\t');     if (!t2) goto next;
            *t2 = 0;
            char *t3 = strchr(t2+1, '\t');     if (!t3) goto next;
            *t3 = 0;
            char info[128];
            snprintf(info, sizeof(info), "群主:%s 人数:%s", t2+1, t3+1);
            GtkTreeIter it;
            gtk_list_store_append(g_search_dlg->store, &it);
            gtk_list_store_set(g_search_dlg->store, &it, 0, gid, 1, t1+1, 2, info, -1);
        }
        next:
        line = strtok_r(NULL, "\n", &save);
    }
    g_free(dup);
}

static void search_do(GtkButton *b, gpointer ud) {
    (void)b;
    SearchDlg *sd = ud;
    const char *q = gtk_entry_get_text(GTK_ENTRY(sd->entry));
    Message m; memset(&m, 0, sizeof(m));
    m.type = sd->is_user ? MSG_USER_SEARCH : MSG_GROUP_SEARCH;
    strncpy(m.body, q, MAX_BODY_LEN - 1); m.body_len = strlen(q);
    net_send(&m);
}

static void search_apply(GtkButton *b, gpointer ud) {
    (void)b;
    SearchDlg *sd = ud;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(sd->view));
    GtkTreeModel *m; GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &m, &it)) {
        msgbox(GTK_WINDOW(sd->dlg), GTK_MESSAGE_WARNING, "请先选择一个目标"); return;
    }
    char hello[256] = {0};
    prompt_text(sd->is_user ? "好友申请" : "入群申请", "附言 (可留空):", hello, sizeof(hello));
    Message req; memset(&req, 0, sizeof(req));
    if (sd->is_user) {
        gchar *name = NULL;
        gtk_tree_model_get(m, &it, 1, &name, -1);
        req.type = MSG_FRIEND_REQ;
        strncpy(req.to_name, name, MAX_NAME_LEN - 1);
        g_free(name);
    } else {
        gint gid = 0;
        gtk_tree_model_get(m, &it, 0, &gid, -1);
        req.type = MSG_GROUP_JOIN_REQ;
        req.group_id = gid;
    }
    strncpy(req.body, hello, MAX_BODY_LEN - 1); req.body_len = strlen(hello);
    net_send(&req);
}

static void search_destroy(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (g_search_dlg) { g_free(g_search_dlg); g_search_dlg = NULL; }
}

static void open_search_dialog(int is_user) {
    GtkWidget *d = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(d), is_user ? "添加好友" : "添加群");
    gtk_window_set_default_size(GTK_WINDOW(d), 460, 420);
    gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(CTX.main_win));
    gtk_window_set_modal(GTK_WINDOW(d), TRUE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(d), vbox);

    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *en = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(en),
        is_user ? "输入用户名关键字" : "输入群名关键字");
    gtk_widget_set_hexpand(en, TRUE);
    GtkWidget *bs = gtk_button_new_with_label("搜索");
    gtk_box_pack_start(GTK_BOX(hb), en, TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(hb), bs, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hb, FALSE, FALSE, 0);

    GtkListStore *store = gtk_list_store_new(3, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    if (is_user) {
        gtk_tree_view_append_column(GTK_TREE_VIEW(view),
            gtk_tree_view_column_new_with_attributes("用户名", gtk_cell_renderer_text_new(), "text", 1, NULL));
        gtk_tree_view_append_column(GTK_TREE_VIEW(view),
            gtk_tree_view_column_new_with_attributes("状态",   gtk_cell_renderer_text_new(), "text", 2, NULL));
    } else {
        gtk_tree_view_append_column(GTK_TREE_VIEW(view),
            gtk_tree_view_column_new_with_attributes("群号", gtk_cell_renderer_text_new(), "text", 0, NULL));
        gtk_tree_view_append_column(GTK_TREE_VIEW(view),
            gtk_tree_view_column_new_with_attributes("群名", gtk_cell_renderer_text_new(), "text", 1, NULL));
        gtk_tree_view_append_column(GTK_TREE_VIEW(view),
            gtk_tree_view_column_new_with_attributes("信息", gtk_cell_renderer_text_new(), "text", 2, NULL));
    }
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), view);
    gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

    GtkWidget *btn = gtk_button_new_with_label(is_user ? "发送好友申请" : "申请加入");
    gtk_style_context_add_class(gtk_widget_get_style_context(btn), "suggested-action");
    gtk_widget_set_size_request(btn, -1, 38);
    gtk_box_pack_start(GTK_BOX(vbox), btn, FALSE, FALSE, 0);

    SearchDlg *sd = g_malloc0(sizeof(*sd));
    sd->is_user = is_user; sd->dlg = d; sd->entry = en; sd->view = view; sd->store = store;
    g_search_dlg = sd;

    g_signal_connect(bs,  "clicked",  G_CALLBACK(search_do),    sd);
    g_signal_connect(en,  "activate", G_CALLBACK(search_do),    sd);
    g_signal_connect(btn, "clicked",  G_CALLBACK(search_apply), sd);
    g_signal_connect(d,   "destroy",  G_CALLBACK(search_destroy), NULL);
    gtk_widget_show_all(d);
}

/* ===================== 创建群 ===================== */
static void open_create_group_dialog(void) {
    char name[64] = {0};
    if (!prompt_text("创建群", "请输入群名:", name, sizeof(name))) return;
    Message m; memset(&m, 0, sizeof(m));
    m.type = MSG_GROUP_CREATE;
    strncpy(m.body, name, MAX_BODY_LEN - 1); m.body_len = strlen(name);
    net_send(&m);
    net_send_text(MSG_GROUP_LIST, NULL, 0, NULL);
}

/* ===================== UI 刷新 (由接收线程通过 g_idle_add) ===================== */
void ui_append_chat(const char *who, const char *time, const char *text) {
    GtkTextIter it;
    gtk_text_buffer_get_end_iter(CTX.chat_buf, &it);
    char line[MAX_BODY_LEN + 128];
    snprintf(line, sizeof(line), "[%s] %s: %s\n",
             time ? time : "", who ? who : "?", text ? text : "");
    gtk_text_buffer_insert(CTX.chat_buf, &it, line, -1);
}

void ui_refresh_friends(const char *body) {
    gtk_list_store_clear(CTX.friend_store);
    if (!body || !*body) return;
    char *dup = g_strdup(body);
    char *save = NULL, *line = strtok_r(dup, "\n", &save);
    while (line) {
        char *t1 = strchr(line, '\t');
        if (t1) {
            *t1 = 0;
            char *t2 = strchr(t1 + 1, '\t');
            int online = atoi(t1 + 1);
            int black  = t2 ? atoi(t2 + 1) : 0;
            GtkTreeIter it;
            gtk_list_store_append(CTX.friend_store, &it);
            gtk_list_store_set(CTX.friend_store, &it, 0, line, 1, online, 2, black, -1);
        }
        line = strtok_r(NULL, "\n", &save);
    }
    g_free(dup);
}

void ui_refresh_groups(const char *body) {
    gtk_list_store_clear(CTX.group_store);
    if (!body || !*body) return;
    char *dup = g_strdup(body);
    char *save = NULL, *line = strtok_r(dup, "\n", &save);
    while (line) {
        char *t1 = strchr(line, '\t');
        if (t1) {
            *t1 = 0;
            int gid = atoi(line);
            char *t2 = strchr(t1 + 1, '\t');
            if (t2) *t2 = 0;
            GtkTreeIter it;
            gtk_list_store_append(CTX.group_store, &it);
            gtk_list_store_set(CTX.group_store, &it, 0, gid, 1, t1 + 1, -1);
        }
        line = strtok_r(NULL, "\n", &save);
    }
    g_free(dup);
}

/* 重新设置通知列表的 tab 文本以显示数量 */
static void refresh_req_tab_label(void) {
    GtkTreeIter it;
    int n = 0;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(CTX.req_store), &it)) {
        do { n++; } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(CTX.req_store), &it));
    }
    char buf[64];
    if (n > 0) snprintf(buf, sizeof(buf), "通知 (%d)", n);
    else       snprintf(buf, sizeof(buf), "通知");
    gtk_label_set_text(GTK_LABEL(CTX.req_count_lbl), buf);
}

/* 取代当前申请列表中所有该 kind 的项. body 为多行文本.
 * kind=0 好友申请, 每行 "reqid\tfrom\ttime\thello"
 * kind=1 入群申请, 每行 "reqid\tgid\tgname\tfrom\ttime\thello" */
void ui_refresh_requests(int kind, const char *body) {
    /* 先删掉同 kind 的所有项 */
    GtkTreeIter it;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(CTX.req_store), &it)) {
        gboolean valid = TRUE;
        while (valid) {
            gint k = 0;
            gtk_tree_model_get(GTK_TREE_MODEL(CTX.req_store), &it, 0, &k, -1);
            if (k == kind) {
                valid = gtk_list_store_remove(CTX.req_store, &it);
            } else {
                valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(CTX.req_store), &it);
            }
        }
    }
    if (!body || !*body) { refresh_req_tab_label(); return; }
    char *dup = g_strdup(body);
    char *save = NULL, *line = strtok_r(dup, "\n", &save);
    while (line) {
        char fields[6][128] = {{0}};
        int n = 0;
        char *p = line, *q;
        while (n < 6 && (q = strchr(p, '\t'))) { *q = 0; strncpy(fields[n++], p, 127); p = q + 1; }
        if (*p && n < 6) strncpy(fields[n++], p, 127);
        char desc[256];
        int reqid = atoi(fields[0]);
        int gid   = 0;
        if (kind == 0) {
            snprintf(desc, sizeof(desc),
                "好友申请: %s    “%s”    %s",
                fields[1], fields[3], fields[2]);
        } else {
            gid = atoi(fields[1]);
            snprintf(desc, sizeof(desc),
                "入群申请: %s 申请加入【%s】 “%s”  %s",
                fields[3], fields[2], fields[5], fields[4]);
        }
        gtk_list_store_append(CTX.req_store, &it);
        gtk_list_store_set(CTX.req_store, &it,
            0, kind, 1, reqid, 2, desc, 3, gid, -1);
        line = strtok_r(NULL, "\n", &save);
    }
    g_free(dup);
    refresh_req_tab_label();
}

void ui_add_request(int kind, int reqid, const char *who, const char *gname,
                    const char *hello, int gid) {
    GtkTreeIter it;
    /* 去重: 同 kind+reqid 已存在则跳过 */
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(CTX.req_store), &it)) {
        do {
            gint k = 0, r = 0;
            gtk_tree_model_get(GTK_TREE_MODEL(CTX.req_store), &it, 0, &k, 1, &r, -1);
            if (k == kind && r == reqid) return;
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(CTX.req_store), &it));
    }
    char desc[256];
    if (kind == 0)
        snprintf(desc, sizeof(desc), "好友申请: %s  “%s”",
                 who ? who : "?", hello ? hello : "");
    else
        snprintf(desc, sizeof(desc), "入群申请: %s 申请加入【%s】  “%s”",
                 who ? who : "?", gname ? gname : "?", hello ? hello : "");
    gtk_list_store_append(CTX.req_store, &it);
    gtk_list_store_set(CTX.req_store, &it, 0, kind, 1, reqid, 2, desc, 3, gid, -1);
    refresh_req_tab_label();
}

void ui_notify_text(const char *title, const char *text) {
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(CTX.main_win),
        GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", title ? title : "");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", text ? text : "");
    g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show_all(d);
}
