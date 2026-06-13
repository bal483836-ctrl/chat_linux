/* =========================================================
 *  chat_linux 客户端 GTK3 界面
 *
 *  布局:
 *    +-- 登录窗 --+    +---------- 主窗 ----------+
 *    | user       |    | header: 当前会话         |
 *    | pass       |    +-----------+-------------+
 *    | host       |    | 好友 / 群 | 聊天滚动区  |
 *    | [登录][注册]|   |           |  输入框      |
 *    +------------+    | [按钮组]  | [发送][文件] |
 *                      +-----------+-------------+
 * ========================================================= */
#include "client.h"
#include "../common/net_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void do_login_clicked(GtkButton *b, gpointer ud);
static void open_register_win(GtkButton *b, gpointer ud);
static void do_register_confirm(GtkButton *b, gpointer ud);
static void on_friend_selected(GtkTreeSelection *sel, gpointer ud);
static void on_group_selected (GtkTreeSelection *sel, gpointer ud);
static void on_send_clicked   (GtkButton *b, gpointer ud);
static void on_addfriend      (GtkButton *b, gpointer ud);
static void on_delfriend      (GtkButton *b, gpointer ud);
static void on_blackadd       (GtkButton *b, gpointer ud);
static void on_blackdel       (GtkButton *b, gpointer ud);
static void on_creategroup    (GtkButton *b, gpointer ud);
static void on_joingroup      (GtkButton *b, gpointer ud);
static void on_sendfile       (GtkButton *b, gpointer ud);
static void on_history        (GtkButton *b, gpointer ud);

/* ===== 工具: 服务器地址 + 关窗时是否退出 ===== */
static const char *server_host(void) {
    const char *h = getenv("CHAT_SERVER_HOST");
    return (h && *h) ? h : "127.0.0.1";
}

/* 关闭登录窗时, 若主窗已切换, 不退出 GTK; 否则退出. */
static gboolean on_login_close(GtkWidget *w, GdkEvent *e, gpointer ud) {
    (void)w; (void)e; (void)ud;
    if (!CTX.main_win) gtk_main_quit();
    return FALSE;
}

/* ===================== 登录窗 ===================== */
void show_login(int argc, char **argv) {
    gtk_init(&argc, &argv);
    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w), "chat_linux 登录");
    gtk_window_set_default_size(GTK_WINDOW(w), 300, 180);
    gtk_window_set_position(GTK_WINDOW(w), GTK_WIN_POS_CENTER);
    g_signal_connect(w, "delete-event", G_CALLBACK(on_login_close), NULL);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 14);
    gtk_container_add(GTK_CONTAINER(w), grid);

    GtkWidget *lu = gtk_label_new("用户名");
    GtkWidget *lp = gtk_label_new("密码");
    GtkWidget *eu = gtk_entry_new();
    GtkWidget *ep = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(ep), FALSE);
    gtk_widget_set_hexpand(eu, TRUE);
    gtk_widget_set_hexpand(ep, TRUE);

    GtkWidget *bl = gtk_button_new_with_label("登录");
    GtkWidget *br = gtk_button_new_with_label("注册");

    gtk_grid_attach(GTK_GRID(grid), lu, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), eu, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), lp, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ep, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), bl, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), br, 1, 2, 1, 1);

    CTX.login_win  = w;
    CTX.login_user = eu;
    CTX.login_pass = ep;
    CTX.login_host = NULL;

    g_signal_connect(bl, "clicked", G_CALLBACK(do_login_clicked),    NULL);
    g_signal_connect(br, "clicked", G_CALLBACK(open_register_win),   NULL);
    /* 回车直接登录 */
    g_signal_connect(ep, "activate", G_CALLBACK(do_login_clicked),   NULL);
    g_signal_connect(eu, "activate", G_CALLBACK(do_login_clicked),   NULL);

    gtk_widget_show_all(w);
    gtk_main();
}

/* 登录 */
static void do_login_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    const char *u = gtk_entry_get_text(GTK_ENTRY(CTX.login_user));
    const char *p = gtk_entry_get_text(GTK_ENTRY(CTX.login_pass));
    if (!*u || !*p) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(CTX.login_win),
            GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
            "请填写用户名和密码");
        gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d);
        return;
    }
    if (CTX.sockfd <= 0 && net_connect(server_host(), SERVER_PORT) < 0) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(CTX.login_win),
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "无法连接服务器 %s:%d\n请确认 chat_server 已启动", server_host(), SERVER_PORT);
        gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d);
        return;
    }
    char body[128]; snprintf(body, sizeof(body), "%s\n%s", u, p);
    Message m; memset(&m, 0, sizeof(m));
    m.type = MSG_LOGIN;
    strncpy(m.body, body, MAX_BODY_LEN - 1); m.body_len = strlen(body);
    net_send(&m);

    Message resp;
    if (recv_msg(CTX.sockfd, &resp) != 0 || resp.type != MSG_RESPONSE || resp.status != RS_OK) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(CTX.login_win),
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "登录失败: %s", resp.body[0] ? resp.body : "用户名或密码错误");
        gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d);
        net_close();
        return;
    }
    strncpy(CTX.username, u, MAX_NAME_LEN - 1);

    /* 先创建主窗 + 隐藏登录窗, 再启动接收线程 — 确保所有 UI 控件就绪 */
    show_main();
    gtk_widget_hide(CTX.login_win);
    pthread_create(&CTX.recv_tid, NULL, recv_thread, NULL);
}

/* ===================== 注册窗 (独立) ===================== */
static GtkWidget *reg_win = NULL;
static GtkWidget *reg_user = NULL;
static GtkWidget *reg_pass = NULL;
static GtkWidget *reg_pass2 = NULL;

static void open_register_win(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    if (reg_win) { gtk_window_present(GTK_WINDOW(reg_win)); return; }

    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w), "chat_linux 注册");
    gtk_window_set_default_size(GTK_WINDOW(w), 320, 220);
    gtk_window_set_position(GTK_WINDOW(w), GTK_WIN_POS_CENTER);
    gtk_window_set_transient_for(GTK_WINDOW(w), GTK_WINDOW(CTX.login_win));
    gtk_window_set_modal(GTK_WINDOW(w), TRUE);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 14);
    gtk_container_add(GTK_CONTAINER(w), grid);

    GtkWidget *lu = gtk_label_new("用户名");
    GtkWidget *lp = gtk_label_new("密码");
    GtkWidget *l2 = gtk_label_new("确认密码");
    reg_user  = gtk_entry_new();
    reg_pass  = gtk_entry_new();
    reg_pass2 = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(reg_pass),  FALSE);
    gtk_entry_set_visibility(GTK_ENTRY(reg_pass2), FALSE);
    gtk_widget_set_hexpand(reg_user, TRUE);

    GtkWidget *bok  = gtk_button_new_with_label("注册");
    GtkWidget *bcan = gtk_button_new_with_label("返回登录");

    gtk_grid_attach(GTK_GRID(grid), lu,        0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), reg_user,  1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), lp,        0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), reg_pass,  1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), l2,        0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), reg_pass2, 1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), bok,       0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), bcan,      1, 3, 1, 1);

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
    if (!*u || !*p) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(reg_win),
            GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
            "用户名和密码不能为空");
        gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d); return;
    }
    if (strcmp(p, p2) != 0) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(reg_win),
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "两次密码输入不一致");
        gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d); return;
    }
    if (CTX.sockfd <= 0 && net_connect(server_host(), SERVER_PORT) < 0) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(reg_win),
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
            "无法连接服务器 %s:%d", server_host(), SERVER_PORT);
        gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d); return;
    }
    char body[128]; snprintf(body, sizeof(body), "%s\n%s", u, p);
    Message m; memset(&m, 0, sizeof(m));
    m.type = MSG_REGISTER;
    strncpy(m.body, body, MAX_BODY_LEN - 1); m.body_len = strlen(body);
    net_send(&m);
    Message resp;
    if (recv_msg(CTX.sockfd, &resp) != 0) return;
    int ok = (resp.type == MSG_RESPONSE && resp.status == RS_OK);
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(reg_win),
        GTK_DIALOG_MODAL,
        ok ? GTK_MESSAGE_INFO : GTK_MESSAGE_ERROR,
        GTK_BUTTONS_OK, "%s", ok ? "注册成功, 可以返回登录" : "注册失败: 用户已存在或服务器拒绝");
    gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d);
    if (ok) {
        /* 把注册的用户名回填到登录窗, 关闭注册窗 */
        gtk_entry_set_text(GTK_ENTRY(CTX.login_user), u);
        gtk_entry_set_text(GTK_ENTRY(CTX.login_pass), "");
        gtk_widget_destroy(reg_win);
    }
}

/* ===================== 主窗 ===================== */
void show_main(void) {
    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    char title[128];
    snprintf(title, sizeof(title), "chat_linux - %s", CTX.username);
    gtk_window_set_title(GTK_WINDOW(w), title);
    gtk_window_set_default_size(GTK_WINDOW(w), 820, 540);
    g_signal_connect(w, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    CTX.main_win = w;

    GtkWidget *hpane = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_container_add(GTK_CONTAINER(w), hpane);

    /* === 左侧: notebook (好友/群) + 操作按钮 === */
    GtkWidget *lvbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *nb = gtk_notebook_new();

    /* 好友 */
    CTX.friend_store = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);
    CTX.friend_view  = gtk_tree_view_new_with_model(GTK_TREE_MODEL(CTX.friend_store));
    GtkCellRenderer *r1 = gtk_cell_renderer_text_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(CTX.friend_view),
        gtk_tree_view_column_new_with_attributes("好友", r1, "text", 0, NULL));
    GtkCellRenderer *r2 = gtk_cell_renderer_toggle_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(CTX.friend_view),
        gtk_tree_view_column_new_with_attributes("在线", r2, "active", 1, NULL));
    GtkCellRenderer *r3 = gtk_cell_renderer_toggle_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(CTX.friend_view),
        gtk_tree_view_column_new_with_attributes("黑名单", r3, "active", 2, NULL));
    GtkWidget *fs = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(fs), CTX.friend_view);
    gtk_widget_set_size_request(fs, 220, 300);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(CTX.friend_view)),
                     "changed", G_CALLBACK(on_friend_selected), NULL);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), fs, gtk_label_new("好友"));

    /* 群组 */
    CTX.group_store = gtk_list_store_new(2, G_TYPE_INT, G_TYPE_STRING);
    CTX.group_view  = gtk_tree_view_new_with_model(GTK_TREE_MODEL(CTX.group_store));
    GtkCellRenderer *g1 = gtk_cell_renderer_text_new();
    GtkCellRenderer *g2 = gtk_cell_renderer_text_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(CTX.group_view),
        gtk_tree_view_column_new_with_attributes("群号", g1, "text", 0, NULL));
    gtk_tree_view_append_column(GTK_TREE_VIEW(CTX.group_view),
        gtk_tree_view_column_new_with_attributes("群名", g2, "text", 1, NULL));
    GtkWidget *gs = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(gs), CTX.group_view);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(CTX.group_view)),
                     "changed", G_CALLBACK(on_group_selected), NULL);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), gs, gtk_label_new("群组"));

    gtk_box_pack_start(GTK_BOX(lvbox), nb, TRUE, TRUE, 0);

    /* 操作按钮 (用网格布局, 2 列) */
    GtkWidget *btn_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(btn_grid), 3);
    gtk_grid_set_column_spacing(GTK_GRID(btn_grid), 3);
    GtkWidget *b_af  = gtk_button_new_with_label("加好友");
    GtkWidget *b_df  = gtk_button_new_with_label("删好友");
    GtkWidget *b_ba  = gtk_button_new_with_label("拉黑");
    GtkWidget *b_bd  = gtk_button_new_with_label("解除");
    GtkWidget *b_cg  = gtk_button_new_with_label("建群");
    GtkWidget *b_jg  = gtk_button_new_with_label("入群");
    GtkWidget *b_his = gtk_button_new_with_label("历史");
    GtkWidget *b_fi  = gtk_button_new_with_label("传文件");
    gtk_grid_attach(GTK_GRID(btn_grid), b_af,  0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(btn_grid), b_df,  1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(btn_grid), b_ba,  0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(btn_grid), b_bd,  1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(btn_grid), b_cg,  0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(btn_grid), b_jg,  1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(btn_grid), b_his, 0, 3, 1, 1);
    gtk_grid_attach(GTK_GRID(btn_grid), b_fi,  1, 3, 1, 1);
    gtk_box_pack_start(GTK_BOX(lvbox), btn_grid, FALSE, FALSE, 0);

    g_signal_connect(b_af,  "clicked", G_CALLBACK(on_addfriend),   NULL);
    g_signal_connect(b_df,  "clicked", G_CALLBACK(on_delfriend),   NULL);
    g_signal_connect(b_ba,  "clicked", G_CALLBACK(on_blackadd),    NULL);
    g_signal_connect(b_bd,  "clicked", G_CALLBACK(on_blackdel),    NULL);
    g_signal_connect(b_cg,  "clicked", G_CALLBACK(on_creategroup), NULL);
    g_signal_connect(b_jg,  "clicked", G_CALLBACK(on_joingroup),   NULL);
    g_signal_connect(b_his, "clicked", G_CALLBACK(on_history),     NULL);
    g_signal_connect(b_fi,  "clicked", G_CALLBACK(on_sendfile),    NULL);

    gtk_paned_add1(GTK_PANED(hpane), lvbox);

    /* === 右侧: 聊天面板 === */
    GtkWidget *rvbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    CTX.chat_header = gtk_label_new("尚未选择会话");
    gtk_label_set_xalign(GTK_LABEL(CTX.chat_header), 0.0);
    gtk_box_pack_start(GTK_BOX(rvbox), CTX.chat_header, FALSE, FALSE, 0);

    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
    CTX.chat_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    GtkWidget *sv = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sv), tv);
    gtk_box_pack_start(GTK_BOX(rvbox), sv, TRUE, TRUE, 0);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    CTX.input_entry = gtk_entry_new();
    GtkWidget *snd  = gtk_button_new_with_label("发送");
    gtk_box_pack_start(GTK_BOX(hbox), CTX.input_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), snd,             FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rvbox), hbox, FALSE, FALSE, 0);

    g_signal_connect(snd, "clicked", G_CALLBACK(on_send_clicked), NULL);
    g_signal_connect(CTX.input_entry, "activate", G_CALLBACK(on_send_clicked), NULL);

    gtk_paned_add2(GTK_PANED(hpane), rvbox);
    gtk_paned_set_position(GTK_PANED(hpane), 240);

    gtk_widget_show_all(w);

    /* 主动拉一次好友列表/群列表(虽然 server 在登录后会主动推, 兜个底) */
    net_send_text(MSG_FRIEND_LIST, NULL, 0, NULL);
    net_send_text(MSG_GROUP_LIST,  NULL, 0, NULL);
}

/* ===================== 列表回调 ===================== */
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
    char hdr[128]; snprintf(hdr, sizeof(hdr), "私聊: %s", name);
    gtk_label_set_text(GTK_LABEL(CTX.chat_header), hdr);
    /* 清空对话区 + 拉历史 */
    gtk_text_buffer_set_text(CTX.chat_buf, "", -1);
    net_send_text(MSG_HISTORY_PRIV, name, 0, NULL);
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
    char hdr[128]; snprintf(hdr, sizeof(hdr), "群聊: %s (#%d)", name ? name : "", gid);
    gtk_label_set_text(GTK_LABEL(CTX.chat_header), hdr);
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
        return;
    }
    char ts[32]; fill_timestamp(ts, sizeof(ts));
    char me[64]; snprintf(me, sizeof(me), "%s (我)", CTX.username);
    ui_append_chat(me, ts, text);
    gtk_entry_set_text(GTK_ENTRY(CTX.input_entry), "");
}

/* ===================== 小工具: 让用户输入一个字符串 ===================== */
static int prompt_input(const char *title, char *out, int outsz) {
    GtkWidget *d = gtk_dialog_new_with_buttons(title, GTK_WINDOW(CTX.main_win),
        GTK_DIALOG_MODAL, "确定", GTK_RESPONSE_OK, "取消", GTK_RESPONSE_CANCEL, NULL);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(d));
    GtkWidget *e = gtk_entry_new();
    gtk_container_add(GTK_CONTAINER(area), e);
    gtk_widget_show_all(d);
    int rc = gtk_dialog_run(GTK_DIALOG(d));
    int ok = 0;
    if (rc == GTK_RESPONSE_OK) {
        strncpy(out, gtk_entry_get_text(GTK_ENTRY(e)), outsz - 1);
        out[outsz - 1] = 0;
        ok = (*out != 0);
    }
    gtk_widget_destroy(d);
    return ok;
}

/* ===================== 各按钮回调 ===================== */
static void on_addfriend(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    char name[MAX_NAME_LEN];
    if (!prompt_input("输入要添加的用户名", name, sizeof(name))) return;
    net_send_text(MSG_FRIEND_ADD, name, 0, NULL);
}
static void on_delfriend(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    if (!*CTX.peer_name) return;
    net_send_text(MSG_FRIEND_DEL, CTX.peer_name, 0, NULL);
}
static void on_blackadd(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    if (!*CTX.peer_name) return;
    net_send_text(MSG_BLACK_ADD, CTX.peer_name, 0, NULL);
}
static void on_blackdel(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    if (!*CTX.peer_name) return;
    net_send_text(MSG_BLACK_DEL, CTX.peer_name, 0, NULL);
}
static void on_creategroup(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    char name[MAX_NAME_LEN];
    if (!prompt_input("新建群名", name, sizeof(name))) return;
    Message m; memset(&m, 0, sizeof(m));
    m.type = MSG_GROUP_CREATE;
    strncpy(m.body, name, MAX_BODY_LEN - 1); m.body_len = strlen(name);
    net_send(&m);
    net_send_text(MSG_GROUP_LIST, NULL, 0, NULL);
}
static void on_joingroup(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    char s[32];
    if (!prompt_input("输入要加入的群号", s, sizeof(s))) return;
    int gid = atoi(s);
    Message m; memset(&m, 0, sizeof(m));
    m.type = MSG_GROUP_JOIN; m.group_id = gid;
    net_send(&m);
    net_send_text(MSG_GROUP_LIST, NULL, 0, NULL);
}
static void on_history(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    gtk_text_buffer_set_text(CTX.chat_buf, "", -1);
    if (CTX.peer_is_group) {
        Message m; memset(&m, 0, sizeof(m));
        m.type = MSG_HISTORY_GROUP; m.group_id = CTX.peer_group_id;
        net_send(&m);
    } else if (*CTX.peer_name) {
        net_send_text(MSG_HISTORY_PRIV, CTX.peer_name, 0, NULL);
    }
}

/* 文件传输: 私聊对方在线时, 分块发送 */
static void on_sendfile(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    if (CTX.peer_is_group || !*CTX.peer_name) return;
    GtkWidget *fc = gtk_file_chooser_dialog_new("选择文件", GTK_WINDOW(CTX.main_win),
        GTK_FILE_CHOOSER_ACTION_OPEN, "取消", GTK_RESPONSE_CANCEL,
        "打开", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(fc)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(fc); return;
    }
    char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
    gtk_widget_destroy(fc);
    if (!path) return;
    FILE *fp = fopen(path, "rb");
    if (!fp) { g_free(path); return; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);

    const char *fname = strrchr(path, '/'); fname = fname ? fname + 1 : path;
    Message m; memset(&m, 0, sizeof(m));
    m.type = MSG_FILE_BEGIN;
    m.status = (uint32_t)sz;
    strncpy(m.to_name, CTX.peer_name, MAX_NAME_LEN - 1);
    strncpy(m.body, fname, MAX_BODY_LEN - 1);
    m.body_len = strlen(fname);
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
    memset(&m, 0, sizeof(m));
    m.type = MSG_FILE_END;
    strncpy(m.to_name, CTX.peer_name, MAX_NAME_LEN - 1);
    net_send(&m);

    fclose(fp);
    char ts[32]; fill_timestamp(ts, sizeof(ts));
    char log[256]; snprintf(log, sizeof(log), "已发送文件 %s (%ld 字节)", fname, sz);
    ui_append_chat("[文件]", ts, log);
    g_free(path);
}

/* ===================== 由接收线程通过 g_idle_add 调用 ===================== */
void ui_append_chat(const char *who, const char *time, const char *text) {
    GtkTextIter it;
    gtk_text_buffer_get_end_iter(CTX.chat_buf, &it);
    char line[MAX_BODY_LEN + 128];
    snprintf(line, sizeof(line), "[%s] %s: %s\n", time ? time : "", who ? who : "?", text ? text : "");
    gtk_text_buffer_insert(CTX.chat_buf, &it, line, -1);
}

void ui_refresh_friends(const char *body) {
    gtk_list_store_clear(CTX.friend_store);
    if (!body || !*body) return;
    char *dup = g_strdup(body);
    char *save = NULL;
    char *line = strtok_r(dup, "\n", &save);
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
    char *save = NULL;
    char *line = strtok_r(dup, "\n", &save);
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
