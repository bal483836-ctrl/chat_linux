/* =========================================================
 *  chat_linux 客户端 GTK3 界面 (主风格 v2)
 *
 *  关键改造:
 *   - 账号登录 (6 位数), 不再用用户名
 *   - 注册带昵称, 服务器返回新分配账号
 *   - 全套 CSS 皮肤 (主蓝, 圆角, 悬浮态)
 *   - Cairo 圆形头像, 颜色来自 0..9 调色板
 *   - 好友/群/通知 三个 GtkListBox 自绘行
 * ========================================================= */
#include "client.h"
#include "../common/net_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------- 调色板 ---------- */
static const struct { double r, g, b; } AVATAR_COLORS[10] = {
    { 0.90, 0.30, 0.36 }, /* 红     */
    { 0.96, 0.42, 0.26 }, /* 橙     */
    { 0.97, 0.72, 0.11 }, /* 黄     */
    { 0.44, 0.80, 0.44 }, /* 绿     */
    { 0.31, 0.76, 0.93 }, /* 浅蓝   */
    { 0.07, 0.72, 0.96 }, /* 主蓝  */
    { 0.29, 0.42, 0.91 }, /* 蓝紫   */
    { 0.61, 0.44, 0.91 }, /* 紫     */
    { 0.91, 0.44, 0.76 }, /* 粉     */
    { 0.56, 0.61, 0.66 }, /* 灰     */
};

/* ---- 前向声明 ---- */
static void do_login_clicked(GtkButton *, gpointer);
static void open_register_win(GtkButton *, gpointer);
static void do_register_confirm(GtkButton *, gpointer);
static void on_send_clicked(GtkButton *, gpointer);
static void on_sendfile(GtkButton *, gpointer);
static void on_add_clicked(GtkButton *, gpointer);
static void open_search_dialog(int is_user);
static void open_create_group_dialog(void);
static void switch_chat_target(int is_group, const char *acc_or_gname,
                               const char *nick, int color, int gid);

/* ---------- 工具 ---------- */
static const char *server_host(void) {
    const char *h = getenv("CHAT_SERVER_HOST");
    return (h && *h) ? h : "127.0.0.1";
}

static gboolean on_login_close(GtkWidget *w, GdkEvent *e, gpointer ud) {
    (void)w; (void)e; (void)ud;
    if (!CTX.main_win) gtk_main_quit();
    return FALSE;
}

static void msgbox(GtkWindow *parent, GtkMessageType type, const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    GtkWidget *d = gtk_message_dialog_new(parent, GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s", buf);
    gtk_dialog_run(GTK_DIALOG(d)); gtk_widget_destroy(d);
}

/* 输入对话框 */
static int prompt_text(const char *title, const char *prompt, char *out, int outsz) {
    GtkWidget *d = gtk_dialog_new_with_buttons(title, GTK_WINDOW(CTX.main_win),
        GTK_DIALOG_MODAL, "取消", GTK_RESPONSE_CANCEL, "确定", GTK_RESPONSE_OK, NULL);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(d));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *lbl = gtk_label_new(prompt);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    GtkWidget *ent = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(ent), TRUE);
    gtk_widget_set_size_request(ent, 280, -1);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ent, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(area), box);
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

/* ---------- 头像绘制 ---------- */

/* 把 UTF-8 字符串首个字符复制到 out (最多 4 字节 + 终止符) */
static void utf8_first(const char *s, char *out, int outsz) {
    if (!s || !*s) { strncpy(out, "?", outsz); return; }
    unsigned char c = (unsigned char)s[0];
    int n = 1;
    if      ((c & 0x80) == 0)    n = 1;
    else if ((c & 0xE0) == 0xC0) n = 2;
    else if ((c & 0xF0) == 0xE0) n = 3;
    else if ((c & 0xF8) == 0xF0) n = 4;
    if (n >= outsz) n = outsz - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

/* AvatarCtx 挂在 GtkDrawingArea 上, draw_avatar 回调读它来知道
 * 该画哪个字、什么颜色, 以及是哪个账号 (用于查头像缓存). */
typedef struct AvatarCtx_ {
    char letter[8];                       /* 昵称首字符的 UTF-8 字节序列  */
    int  color;                           /* 调色板下标 0..9             */
    int  size;                            /* 绘制尺寸 (px)               */
    char account[MAX_NAME_LEN];           /* "" 表示不查缓存(纯字母头像) */
} AvatarCtx;

/* 内部: 把 PNG 字节解码成 GdkPixbuf, 调用方 owns 返回值. */
static GdkPixbuf *pixbuf_from_png_bytes(const unsigned char *bytes, int len) {
    if (!bytes || len <= 0) return NULL;
    GdkPixbufLoader *l = gdk_pixbuf_loader_new();
    if (!gdk_pixbuf_loader_write(l, bytes, len, NULL)) { g_object_unref(l); return NULL; }
    gdk_pixbuf_loader_close(l, NULL);
    GdkPixbuf *p = gdk_pixbuf_loader_get_pixbuf(l);
    if (p) g_object_ref(p);    /* loader 拥有 pixbuf, 要 ref 才能跨出去 */
    g_object_unref(l);
    return p;
}

/* DrawingArea draw 信号回调.
 * 思路: 先看 CTX.avatar_cache 里这账号有没有 PNG 头像;
 *       有就用 cairo_set_source_surface 把 pixbuf 画上去, 用圆形 clip 剪边;
 *       没有 (或 account 为空) 就退回到老逻辑: 渐变圆 + 首字母. */
static gboolean draw_avatar(GtkWidget *w, cairo_t *cr, gpointer ud) {
    AvatarCtx *a = ud;
    int ww = gtk_widget_get_allocated_width(w);
    int hh = gtk_widget_get_allocated_height(w);
    int s  = ww < hh ? ww : hh;
    double cx = ww / 2.0, cy = hh / 2.0, r = s / 2.0 - 1;

    /* --- 路径 1: 走 PNG 缓存 ---------------------------------- */
    GdkPixbuf *pb = NULL;
    if (CTX.avatar_cache && a->account[0]) {
        pb = g_hash_table_lookup(CTX.avatar_cache, a->account);
    }
    if (pb) {
        /* 圆形 clip → 把 pixbuf 缩放铺满 */
        cairo_save(cr);
        cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
        cairo_clip(cr);
        int pw = gdk_pixbuf_get_width(pb), ph = gdk_pixbuf_get_height(pb);
        double sx = (double)s / pw, sy = (double)s / ph;
        cairo_translate(cr, cx - s / 2.0, cy - s / 2.0);
        cairo_scale(cr, sx, sy);
        gdk_cairo_set_source_pixbuf(cr, pb, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
        return TRUE;
    }

    /* --- 路径 2: 渐变圆形 + 首字母 (回退) ---------------------- */
    cairo_pattern_t *p = cairo_pattern_create_linear(0, 0, 0, s);
    const struct { double r, g, b; } *c = &AVATAR_COLORS[a->color % 10];
    /* 顶部稍亮、底部稍暗, 给一点立体感 */
    cairo_pattern_add_color_stop_rgb(p, 0, c->r * 1.08, c->g * 1.08, c->b * 1.08);
    cairo_pattern_add_color_stop_rgb(p, 1, c->r * 0.85, c->g * 0.85, c->b * 0.85);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_set_source(cr, p);
    cairo_fill(cr);
    cairo_pattern_destroy(p);

    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, s * 0.55);
    cairo_text_extents_t te;
    cairo_text_extents(cr, a->letter, &te);
    cairo_move_to(cr, cx - te.width / 2 - te.x_bearing, cy + te.height / 2);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.96);
    cairo_show_text(cr, a->letter);
    return TRUE;
}

/* 控件销毁前从 avatar_widgets 反查表里把自己摘掉, 避免悬空指针.
 * GLib 不允许在 destroy 时调用 hashtable 的 remove (因为 value 是 GList),
 * 所以我们直接遍历 list 把当前 DA 节点干掉. */
static void on_avatar_destroyed(GtkWidget *w, gpointer ud) {
    AvatarCtx *a = ud;
    if (CTX.avatar_widgets && a->account[0]) {
        GList *l = g_hash_table_lookup(CTX.avatar_widgets, a->account);
        l = g_list_remove(l, w);
        if (l) g_hash_table_insert(CTX.avatar_widgets, g_strdup(a->account), l);
        else   g_hash_table_remove(CTX.avatar_widgets, a->account);
    }
    g_free(a);
}

GtkWidget *avatar_widget(const char *account, const char *nick, int color, int size) {
    AvatarCtx *a = g_malloc0(sizeof(*a));
    utf8_first(nick && *nick ? nick : "?", a->letter, sizeof(a->letter));
    /* ASCII 小写转大写, 让头像字符更醒目; UTF-8 中文字符保持不动 */
    if ((unsigned char)a->letter[0] < 0x80 && a->letter[0] >= 'a' && a->letter[0] <= 'z')
        a->letter[0] -= 32;
    a->color = color;
    a->size  = size;
    if (account && *account) strncpy(a->account, account, MAX_NAME_LEN - 1);

    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, size, size);
    g_signal_connect(da, "draw",    G_CALLBACK(draw_avatar),         a);
    g_signal_connect(da, "destroy", G_CALLBACK(on_avatar_destroyed), a);

    /* 注册到反查表: 等头像到了之后批量 queue_draw. */
    if (CTX.avatar_widgets && a->account[0]) {
        GList *l = g_hash_table_lookup(CTX.avatar_widgets, a->account);
        l = g_list_prepend(l, da);
        g_hash_table_insert(CTX.avatar_widgets, g_strdup(a->account), l);
    }
    /* 顺手发一次 GET; avatar_request_if_needed 内部做了去重. */
    if (a->account[0]) avatar_request_if_needed(a->account);
    return da;
}

/* 收到 MSG_AVATAR_DATA 后调用. 字节为空表示对方没传过头像,
 * 我们也要进缓存 (NULL 标记), 不然 avatar_widget 每次创建都会再发 GET. */
void avatar_cache_put(const char *account, const unsigned char *png, int len) {
    if (!account || !*account || !CTX.avatar_cache) return;
    GdkPixbuf *pb = pixbuf_from_png_bytes(png, len);
    if (pb) {
        g_hash_table_replace(CTX.avatar_cache, g_strdup(account), pb);
    } else {
        /* 标记为"已知没有", 防止重复请求 */
        g_hash_table_remove(CTX.avatar_cache, account);
    }
    /* 触发所有显示此账号的头像 widget 重绘 */
    GList *l = g_hash_table_lookup(CTX.avatar_widgets, account);
    for (; l; l = l->next) gtk_widget_queue_draw(GTK_WIDGET(l->data));
}

void avatar_request_if_needed(const char *account) {
    if (!account || !*account) return;
    if (!CTX.avatar_requested) return;
    if (g_hash_table_contains(CTX.avatar_requested, account)) return;
    g_hash_table_add(CTX.avatar_requested, g_strdup(account));
    Message m; memset(&m, 0, sizeof(m));
    m.type = MSG_AVATAR_GET;
    strncpy(m.to_name, account, MAX_NAME_LEN - 1);
    net_send(&m);
}

/* ---------- CSS 全局皮肤 ---------- */
static void apply_css(void) {
    /* 全局皮肤. 设计原则:
     *   - 浅蓝灰底, 白色卡片承载内容, 主色 #12b7f5 用在 header / 主按钮
     *   - 圆角 6~10px, 列表行 hover/selected 用主色低透明度
     *   - 按钮用线性渐变模拟"光感", hover 时颜色加深
     *   - 输入框统一灰边框, 聚焦时切到主色
     * 备注: GTK CSS 是 CSS 子集, 不支持 box-shadow, 我们改用 1px 边框
     * + 微弱的背景对比来代替阴影. */
    static const char *CSS =
    "window { background-color: #eef2f8; }\n"
    ".im-card { background-color: #ffffff; border-radius: 10px;\n"
    "           border: 1px solid #e5e7eb; }\n"
    ".im-sidebar { background-color: #ffffff; border-right: 1px solid #e5e7eb; }\n"
    ".im-header { background: linear-gradient(180deg, #2eb1ee, #12b7f5);\n"
    "             color: white; padding: 12px 14px; }\n"
    ".im-header label { color: white; font-weight: bold; }\n"
    ".im-self {\n"
    "    background-color: #ffffff;\n"
    "    border-bottom: 1px solid #e5e7eb;\n"
    "    padding: 14px 12px;\n"
    "}\n"
    ".im-row {\n"
    "    padding: 10px 12px;\n"
    "    border-bottom: 1px solid #f1f5f9;\n"
    "    transition: background-color 120ms ease-in-out;\n"
    "}\n"
    ".im-row:selected, .im-row:selected:focus {\n"
    "    background-color: rgba(18,183,245,0.18);\n"
    "}\n"
    ".im-row:hover { background-color: rgba(18,183,245,0.08); }\n"
    ".im-nick { font-size: 11pt; font-weight: 600; color: #1f2937; }\n"
    ".im-acc  { font-size: 9pt;  color: #94a3b8; }\n"
    ".im-online  { color: #4caf50; font-weight: bold; }\n"
    ".im-offline { color: #c0c7d0; }\n"
    ".im-primary {\n"
    "    background: linear-gradient(180deg, #38bdf8, #0ea5e9);\n"
    "    color: white;\n"
    "    border-radius: 6px;\n"
    "    border: none;\n"
    "    padding: 8px 16px;\n"
    "    font-weight: bold;\n"
    "    transition: background-image 120ms ease-in-out;\n"
    "}\n"
    ".im-primary:hover { background: linear-gradient(180deg, #38bdf8, #0284c7); }\n"
    ".im-primary:active { background: linear-gradient(180deg, #0ea5e9, #0369a1); }\n"
    ".im-secondary {\n"
    "    background-color: #f1f5f9;\n"
    "    color: #475569;\n"
    "    border-radius: 6px;\n"
    "    border: 1px solid #e2e8f0;\n"
    "    padding: 6px 12px;\n"
    "    transition: background-color 120ms ease-in-out;\n"
    "}\n"
    ".im-secondary:hover { background-color: #e2e8f0; }\n"
    ".im-iconbtn {\n"
    "    background-color: rgba(255,255,255,0.22);\n"
    "    border-radius: 50%;\n"
    "    border: none;\n"
    "    color: white;\n"
    "    font-size: 14pt;\n"
    "    min-width: 30px; min-height: 30px;\n"
    "}\n"
    ".im-iconbtn:hover { background-color: rgba(255,255,255,0.38); }\n"
    /* 自卡片上的"添加"按钮: 白底场景, 用主色实心圆 + 白色加号 */
    ".im-addbtn-self {\n"
    "    background: linear-gradient(180deg, #38bdf8, #0ea5e9);\n"
    "    color: white;\n"
    "    border-radius: 50%;\n"
    "    border: none;\n"
    "    font-weight: bold;\n"
    "    font-size: 14pt;\n"
    "    min-width: 36px; min-height: 36px;\n"
    "    padding: 0;\n"
    "    transition: background-image 120ms ease-in-out;\n"
    "}\n"
    ".im-addbtn-self:hover { background: linear-gradient(180deg, #38bdf8, #0284c7); }\n"
    ".im-addbtn-self:active { background: linear-gradient(180deg, #0ea5e9, #0369a1); }\n"
    "entry {\n"
    "    border-radius: 6px;\n"
    "    border: 1px solid #cbd5e1;\n"
    "    padding: 6px 10px;\n"
    "    background-color: #ffffff;\n"
    "    transition: border-color 120ms ease-in-out;\n"
    "}\n"
    "entry:focus { border-color: #12b7f5; }\n"
    "notebook header tabs tab { padding: 9px 16px; color: #64748b; }\n"
    "notebook header tabs tab:checked {\n"
    "    background-color: #ffffff;\n"
    "    color: #12b7f5;\n"
    "    border-bottom: 2px solid #12b7f5;\n"
    "    font-weight: bold;\n"
    "}\n"
    "textview text { background-color: #f8fafc; }\n"
    "textview { padding: 4px; }\n";

    GtkCssProvider *p = gtk_css_provider_new();
    GError *err = NULL;
    gtk_css_provider_load_from_data(p, CSS, -1, &err);
    if (err) { g_warning("CSS error: %s", err->message); g_error_free(err); }
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(p), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

/* ===================== 登录窗 ===================== */
void show_login(int argc, char **argv) {
    gtk_init(&argc, &argv);
    apply_css();
    /* 头像三件套初始化:
     *   avatar_cache    : account -> GdkPixbuf* (有图就显示)
     *   avatar_requested: 已发过 GET 的账号集合 (避免重复请求)
     *   avatar_widgets  : account -> GList<GtkDrawingArea*> 反查表 */
    CTX.avatar_cache     = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    CTX.avatar_requested = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    CTX.avatar_widgets   = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w), "chat_linux 登录");
    /* 窗体放大一些, 视觉更舒展; 也给后续往里加 "记住密码" / "找回密码"
     * 之类的功能留点空间 */
    gtk_window_set_default_size(GTK_WINDOW(w), 460, 380);
    gtk_window_set_position(GTK_WINDOW(w), GTK_WIN_POS_CENTER);
    gtk_window_set_resizable(GTK_WINDOW(w), FALSE);
    g_signal_connect(w, "delete-event", G_CALLBACK(on_login_close), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 24);
    gtk_container_add(GTK_CONTAINER(w), vbox);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title),
        "<span size='xx-large' weight='bold' color='#12b7f5'>chat_linux</span>");
    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 8);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);

    GtkWidget *lu = gtk_label_new("账  号");
    GtkWidget *lp = gtk_label_new("密  码");
    GtkWidget *eu = gtk_entry_new();
    GtkWidget *ep = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(eu), "账号 (如 100001)");
    gtk_entry_set_placeholder_text(GTK_ENTRY(ep), "请输入密码");
    gtk_entry_set_input_purpose(GTK_ENTRY(eu), GTK_INPUT_PURPOSE_DIGITS);
    gtk_entry_set_visibility(GTK_ENTRY(ep), FALSE);
    gtk_widget_set_size_request(eu, 200, -1);
    gtk_grid_attach(GTK_GRID(grid), lu, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), eu, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), lp, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ep, 1, 1, 1, 1);

    /* 按钮: 注册(小,左) | 登录(大,右,主色) */
    GtkWidget *btnbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *br = gtk_button_new_with_label("注册");
    GtkWidget *bl = gtk_button_new_with_label("登 录");
    gtk_widget_set_size_request(br, 80, 40);
    gtk_widget_set_size_request(bl, 200, 44);
    gtk_style_context_add_class(gtk_widget_get_style_context(br), "im-secondary");
    gtk_style_context_add_class(gtk_widget_get_style_context(bl), "im-primary");
    gtk_box_pack_start(GTK_BOX(btnbox), br, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btnbox), bl, TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(vbox), btnbox, FALSE, FALSE, 6);

    CTX.login_win  = w;
    CTX.login_user = eu;
    CTX.login_pass = ep;

    g_signal_connect(bl, "clicked",  G_CALLBACK(do_login_clicked),  NULL);
    g_signal_connect(br, "clicked",  G_CALLBACK(open_register_win), NULL);
    g_signal_connect(eu, "activate", G_CALLBACK(do_login_clicked),  NULL);
    g_signal_connect(ep, "activate", G_CALLBACK(do_login_clicked),  NULL);

    gtk_widget_show_all(w);
    gtk_main();
}

static void do_login_clicked(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    const char *acc = gtk_entry_get_text(GTK_ENTRY(CTX.login_user));
    const char *p   = gtk_entry_get_text(GTK_ENTRY(CTX.login_pass));
    if (!*acc || !*p) { msgbox(GTK_WINDOW(CTX.login_win), GTK_MESSAGE_WARNING, "请填写账号和密码"); return; }
    if (CTX.sockfd <= 0 && net_connect(server_host(), SERVER_PORT) < 0) {
        msgbox(GTK_WINDOW(CTX.login_win), GTK_MESSAGE_ERROR,
            "无法连接服务器 %s:%d\n请确认 chat_server 已启动", server_host(), SERVER_PORT);
        return;
    }
    char body[128]; snprintf(body, sizeof(body), "%s\n%s", acc, p);
    Message m; memset(&m, 0, sizeof(m));
    m.type = MSG_LOGIN;
    strncpy(m.body, body, MAX_BODY_LEN - 1); m.body_len = strlen(body);
    net_send(&m);
    Message resp;
    if (recv_msg(CTX.sockfd, &resp) != 0 || resp.type != MSG_RESPONSE || resp.status != RS_OK) {
        msgbox(GTK_WINDOW(CTX.login_win), GTK_MESSAGE_ERROR,
            "登录失败: %s", resp.body[0] ? resp.body : "账号或密码错误");
        net_close();
        return;
    }
    /* body = "account\nnickname" */
    char *nl = strchr(resp.body, '\n');
    if (nl) {
        *nl = 0;
        strncpy(CTX.account,  resp.body, MAX_NAME_LEN - 1);
        strncpy(CTX.nickname, nl + 1,    MAX_NAME_LEN - 1);
    }
    int color = 0;
    for (const char *q = CTX.nickname; *q; ++q) color = (color * 131 + (unsigned char)*q) & 0xFFFF;
    CTX.avatar_color = color % 10;
    show_main();
    gtk_widget_hide(CTX.login_win);
    pthread_create(&CTX.recv_tid, NULL, recv_thread, NULL);
}

/* ===================== 注册窗 ===================== */
/* 注册窗内部用的临时变量, 单例.
 * reg_preview_pb 是当前预览中的 GdkPixbuf, 用户每次重选都会替换.
 * 这块在 do_register_confirm 里被读, 在窗口 destroy 时一起清掉. */
static GtkWidget *reg_win        = NULL;
static GtkWidget *reg_nick       = NULL;
static GtkWidget *reg_pass       = NULL;
static GtkWidget *reg_pass2      = NULL;
static GtkWidget *reg_preview    = NULL;     /* GtkImage, 显示选中的头像缩略  */
static GdkPixbuf *reg_preview_pb = NULL;     /* 已 scale 到 64x64 的 RGBA 数据*/

/* 用户点击"选择头像"按钮 → 弹文件选择器 → 读图 → scale 到 64x64 → 入 preview.
 * 关键点:
 *   1) gdk_pixbuf_new_from_file_at_scale 一步完成解码+缩放, 比先解码后缩放省内存
 *   2) 我们存 RGBA 的 pixbuf 而不是 PNG 字节, 因为注册时还没拿到账号,
 *      要到拿到账号后才知道往 data/avatars/<id>.png 写, 所以延迟到那时再编码 PNG */
static void on_pick_avatar(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    GtkWidget *fc = gtk_file_chooser_dialog_new("选择头像图片", GTK_WINDOW(reg_win),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "取消", GTK_RESPONSE_CANCEL,
        "打开", GTK_RESPONSE_ACCEPT, NULL);
    /* 只列出常见图片格式; GTK 自带 filter 支持 mime-type 通配 */
    GtkFileFilter *flt = gtk_file_filter_new();
    gtk_file_filter_set_name(flt, "图片 (png/jpg/bmp)");
    gtk_file_filter_add_mime_type(flt, "image/png");
    gtk_file_filter_add_mime_type(flt, "image/jpeg");
    gtk_file_filter_add_mime_type(flt, "image/bmp");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(fc), flt);

    if (gtk_dialog_run(GTK_DIALOG(fc)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(fc); return;
    }
    char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
    gtk_widget_destroy(fc);
    if (!path) return;

    /* 64x64 是头像的目标尺寸: 既保证显示清晰, 又能压成 <4KB PNG 一次性传输.
     * 第 4 参数 TRUE = 保持纵横比 (短边对齐, 多余裁切) — 实际是 fit, 我们
     * 在 draw_avatar 里再做圆形 clip. */
    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path, 64, 64, TRUE, &err);
    g_free(path);
    if (!pb) {
        msgbox(GTK_WINDOW(reg_win), GTK_MESSAGE_ERROR,
               "无法解析所选图片: %s", err ? err->message : "未知错误");
        if (err) g_error_free(err);
        return;
    }
    if (reg_preview_pb) g_object_unref(reg_preview_pb);
    reg_preview_pb = pb;
    gtk_image_set_from_pixbuf(GTK_IMAGE(reg_preview), reg_preview_pb);
}

/* 用户改了昵称且还没选头像时, 同步更新预览的首字母圆形 */
static void on_reg_nick_changed(GtkEditable *e, gpointer ud) {
    (void)ud;
    if (reg_preview_pb) return;          /* 已选图就别覆盖 */
    /* 这里偷个懒: 不重画, 直接换 image; 实际上我们让 image 显示 NULL,
     * 改用旁边一个静态字母 label 即可. 简化版本直接保持不变. */
    (void)e;
}

/* 释放预览 pixbuf, 避免窗口销毁后内存泄漏 */
static void on_reg_destroyed(GtkWidget *w, gpointer ud) {
    (void)w; (void)ud;
    if (reg_preview_pb) { g_object_unref(reg_preview_pb); reg_preview_pb = NULL; }
    reg_win = reg_nick = reg_pass = reg_pass2 = reg_preview = NULL;
}

static void open_register_win(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    if (reg_win) { gtk_window_present(GTK_WINDOW(reg_win)); return; }
    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w), "chat_linux 注册");
    /* 注册窗有头像选择 + 三个输入框, 比登录窗高一些 */
    gtk_window_set_default_size(GTK_WINDOW(w), 520, 540);
    gtk_window_set_position(GTK_WINDOW(w), GTK_WIN_POS_CENTER);
    gtk_window_set_transient_for(GTK_WINDOW(w), GTK_WINDOW(CTX.login_win));
    gtk_window_set_modal(GTK_WINDOW(w), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(w), FALSE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 24);
    gtk_container_add(GTK_CONTAINER(w), vbox);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title),
        "<span size='x-large' weight='bold' color='#12b7f5'>欢迎注册</span>");
    gtk_box_pack_start(GTK_BOX(vbox), title, FALSE, FALSE, 4);
    GtkWidget *sub = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(sub),
        "<span color='#94a3b8' size='small'>注册成功后系统将自动为你分配账号</span>");
    gtk_box_pack_start(GTK_BOX(vbox), sub, FALSE, FALSE, 0);

    /* === 头像选择区 ===
     * 左边一个 64x64 GtkImage 显示预览; 右边一个"选择头像"按钮 */
    GtkWidget *avbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign(avbox, GTK_ALIGN_CENTER);
    reg_preview = gtk_image_new_from_icon_name("avatar-default-symbolic", GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_size_request(reg_preview, 64, 64);
    GtkWidget *pick = gtk_button_new_with_label("选择头像 (可选)");
    gtk_style_context_add_class(gtk_widget_get_style_context(pick), "im-secondary");
    gtk_widget_set_valign(pick, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(avbox), reg_preview, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(avbox), pick,        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), avbox, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 4);

    GtkWidget *l1 = gtk_label_new("昵    称");
    GtkWidget *l2 = gtk_label_new("密    码");
    GtkWidget *l3 = gtk_label_new("确认密码");
    reg_nick  = gtk_entry_new();
    reg_pass  = gtk_entry_new();
    reg_pass2 = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(reg_nick), "中英文均可, 1~16 字符");
    gtk_entry_set_placeholder_text(GTK_ENTRY(reg_pass), "请输入密码");
    gtk_entry_set_placeholder_text(GTK_ENTRY(reg_pass2),"再次输入密码");
    gtk_entry_set_visibility(GTK_ENTRY(reg_pass),  FALSE);
    gtk_entry_set_visibility(GTK_ENTRY(reg_pass2), FALSE);
    gtk_widget_set_size_request(reg_nick, 220, -1);
    gtk_grid_attach(GTK_GRID(grid), l1, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), reg_nick, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), l2, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), reg_pass, 1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), l3, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), reg_pass2, 1, 2, 1, 1);

    GtkWidget *btnbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *bcan = gtk_button_new_with_label("返回");
    GtkWidget *bok  = gtk_button_new_with_label("确认注册");
    gtk_widget_set_size_request(bcan, 80,  40);
    gtk_widget_set_size_request(bok, 220,  44);
    gtk_style_context_add_class(gtk_widget_get_style_context(bcan), "im-secondary");
    gtk_style_context_add_class(gtk_widget_get_style_context(bok),  "im-primary");
    gtk_box_pack_start(GTK_BOX(btnbox), bcan, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btnbox), bok,  TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(vbox), btnbox, FALSE, FALSE, 6);

    g_signal_connect(pick,     "clicked",  G_CALLBACK(on_pick_avatar),       NULL);
    g_signal_connect(reg_nick, "changed",  G_CALLBACK(on_reg_nick_changed),  NULL);
    g_signal_connect(bok,      "clicked",  G_CALLBACK(do_register_confirm),  NULL);
    g_signal_connect_swapped(bcan, "clicked", G_CALLBACK(gtk_widget_destroy), w);
    g_signal_connect(w,        "destroy",  G_CALLBACK(on_reg_destroyed),     NULL);

    reg_win = w;
    gtk_widget_show_all(w);
}

static void do_register_confirm(GtkButton *b, gpointer ud) {
    (void)b; (void)ud;
    const char *nick = gtk_entry_get_text(GTK_ENTRY(reg_nick));
    const char *p    = gtk_entry_get_text(GTK_ENTRY(reg_pass));
    const char *p2   = gtk_entry_get_text(GTK_ENTRY(reg_pass2));
    if (!*nick || !*p) { msgbox(GTK_WINDOW(reg_win), GTK_MESSAGE_WARNING, "昵称和密码不能为空"); return; }
    if (strcmp(p, p2) != 0) { msgbox(GTK_WINDOW(reg_win), GTK_MESSAGE_ERROR, "两次密码输入不一致"); return; }
    if (CTX.sockfd <= 0 && net_connect(server_host(), SERVER_PORT) < 0) {
        msgbox(GTK_WINDOW(reg_win), GTK_MESSAGE_ERROR, "无法连接服务器"); return;
    }
    char body[128]; snprintf(body, sizeof(body), "%s\n%s", nick, p);
    Message m; memset(&m, 0, sizeof(m));
    m.type = MSG_REGISTER;
    strncpy(m.body, body, MAX_BODY_LEN - 1); m.body_len = strlen(body);
    net_send(&m);
    Message resp;
    if (recv_msg(CTX.sockfd, &resp) != 0) return;
    if (resp.type == MSG_RESPONSE && resp.status == RS_OK) {
        /* === 头像上传 ===
         * 用户在表单选了图就在这里编码 PNG 并 MSG_AVATAR_UPLOAD 上去.
         * 注意上传只能在注册成功的那一瞬间做, 因为这时连接还是匿名态,
         * 服务器认 fd 还是认账号都行 — 不过我们的服务器要求"已登录",
         * 所以这里其实是先登录再上传, 见下方两步. */
        if (reg_preview_pb) {
            gchar  *png_buf = NULL;
            gsize   png_sz  = 0;
            /* gdk_pixbuf_save_to_buffer: 把 RGBA pixbuf 编码到 PNG 字节流;
             * compression=9 是最大压缩, 时间换空间 — 头像就一张, 不耗时 */
            if (gdk_pixbuf_save_to_buffer(reg_preview_pb, &png_buf, &png_sz,
                                          "png", NULL, "compression", "9", NULL)
                && png_sz > 0 && png_sz < MAX_BODY_LEN) {
                /* 先登录 (复用刚刚的连接) 才能上传 */
                Message lg; memset(&lg, 0, sizeof(lg));
                lg.type = MSG_LOGIN;
                snprintf(lg.body, sizeof(lg.body), "%s\n%s", resp.body, p);
                lg.body_len = strlen(lg.body);
                net_send(&lg);
                Message lresp;
                if (recv_msg(CTX.sockfd, &lresp) == 0 &&
                    lresp.type == MSG_RESPONSE && lresp.status == RS_OK) {
                    /* 服务端 login 成功后还会推 FLIST/GLIST/REQ 等, 这里我们
                     * 都先吞掉, 再发 AVATAR_UPLOAD; 否则后面新连接再来时
                     * 服务器侧的 fd 状态对不上. 简化: 直接关掉连接, 让用户
                     * 重新点登录, 再上传一次. 但更稳的是接着把头像传完再断. */
                    Message av; memset(&av, 0, sizeof(av));
                    av.type   = MSG_AVATAR_UPLOAD;
                    av.status = (uint32_t)png_sz;
                    memcpy(av.body, png_buf, png_sz);
                    av.body_len = (uint32_t)png_sz;
                    net_send(&av);
                }
                /* 关闭并重置连接, 让用户回到登录窗以正常流程登录 */
                net_close();
            }
            g_free(png_buf);
        }
        msgbox(GTK_WINDOW(reg_win), GTK_MESSAGE_INFO,
               "注册成功!\n您的账号是: %s\n请妥善保管, 登录请使用此账号", resp.body);
        gtk_entry_set_text(GTK_ENTRY(CTX.login_user), resp.body);
        gtk_entry_set_text(GTK_ENTRY(CTX.login_pass), "");
        gtk_widget_grab_focus(CTX.login_pass);
        gtk_widget_destroy(reg_win);
    } else {
        msgbox(GTK_WINDOW(reg_win), GTK_MESSAGE_ERROR,
               "注册失败: %s", resp.body[0] ? resp.body : "服务器拒绝");
    }
}

/* ===================== 行 (好友/群/通知) 构造 ===================== */
/* 用 g_object_set_data 在 row 上挂载 metadata, 供右键菜单读取 */

typedef struct { int kind; int reqid; int gid; char acc[16]; char nick[32]; int color; char hello[256]; char gname[64]; } RowData;

static void row_data_free(RowData *r) { g_free(r); }

static GtkWidget *make_friend_row(const char *acc, const char *nick, int color,
                                  int online, int black) {
    GtkWidget *row = gtk_list_box_row_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "im-row");

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_add(GTK_CONTAINER(row), hbox);
    /* 头像: 传 acc 进去, 让 avatar_widget 能命中缓存 / 自动请求 */
    gtk_box_pack_start(GTK_BOX(hbox), avatar_widget(acc, nick, color, 40), FALSE, FALSE, 0);

    /* 昵称 + 账号 */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *l1 = gtk_label_new(NULL);
    char buf[128];
    snprintf(buf, sizeof(buf),
        "<span class='im-nick' weight='bold' size='medium'>%s</span>%s",
        nick, black ? " <span color='#ef4444' size='small'>[黑]</span>" : "");
    gtk_label_set_markup(GTK_LABEL(l1), buf);
    gtk_label_set_xalign(GTK_LABEL(l1), 0.0);
    GtkWidget *l2 = gtk_label_new(NULL);
    snprintf(buf, sizeof(buf), "<span color='#94a3b8' size='small'>%s</span>", acc);
    gtk_label_set_markup(GTK_LABEL(l2), buf);
    gtk_label_set_xalign(GTK_LABEL(l2), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), l1, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), l2, FALSE, FALSE, 0);
    gtk_widget_set_hexpand(vbox, TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);

    /* 在线指示 */
    GtkWidget *dot = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(dot),
        online ? "<span color='#4caf50' size='x-large'>●</span>"
               : "<span color='#cbd5e1' size='x-large'>●</span>");
    gtk_box_pack_start(GTK_BOX(hbox), dot, FALSE, FALSE, 0);

    RowData *rd = g_malloc0(sizeof(*rd));
    rd->kind = -1;  /* friend */
    strncpy(rd->acc,  acc,  sizeof(rd->acc) - 1);
    strncpy(rd->nick, nick, sizeof(rd->nick) - 1);
    rd->color = color;
    rd->reqid = black ? 1 : 0;       /* 复用此字段表示是否拉黑 */
    g_object_set_data_full(G_OBJECT(row), "rd", rd, (GDestroyNotify)row_data_free);

    gtk_widget_show_all(row);
    return row;
}

static GtkWidget *make_group_row(int gid, const char *name) {
    GtkWidget *row = gtk_list_box_row_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "im-row");
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_add(GTK_CONTAINER(row), hbox);
    /* 群头像: 群没有头像图片, 直接字母 + gid 派生颜色 */
    gtk_box_pack_start(GTK_BOX(hbox), avatar_widget(NULL, name, gid % 10, 40), FALSE, FALSE, 0);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *l1 = gtk_label_new(NULL);
    char buf[128];
    snprintf(buf, sizeof(buf), "<span weight='bold'>%s</span>", name);
    gtk_label_set_markup(GTK_LABEL(l1), buf);
    gtk_label_set_xalign(GTK_LABEL(l1), 0.0);
    GtkWidget *l2 = gtk_label_new(NULL);
    snprintf(buf, sizeof(buf), "<span color='#94a3b8' size='small'>群号 %d</span>", gid);
    gtk_label_set_markup(GTK_LABEL(l2), buf);
    gtk_label_set_xalign(GTK_LABEL(l2), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), l1, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), l2, FALSE, FALSE, 0);
    gtk_widget_set_hexpand(vbox, TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);

    RowData *rd = g_malloc0(sizeof(*rd));
    rd->kind = -2;  /* group */
    rd->gid = gid;
    strncpy(rd->gname, name, sizeof(rd->gname) - 1);
    g_object_set_data_full(G_OBJECT(row), "rd", rd, (GDestroyNotify)row_data_free);

    gtk_widget_show_all(row);
    return row;
}

/* 申请项: 内联同意/拒绝按钮 (避免右键) */
static void on_req_accept(GtkButton *b, gpointer ud);
static void on_req_reject(GtkButton *b, gpointer ud);

static GtkWidget *make_req_row(int kind, int reqid, const char *acc, const char *nick,
                               int color, const char *gname, const char *hello, int gid) {
    GtkWidget *row = gtk_list_box_row_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "im-row");
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_container_add(GTK_CONTAINER(row), hbox);
    /* 申请项的头像也走缓存: 入群申请的 acc 为 "" 时退化为字母 */
    gtk_box_pack_start(GTK_BOX(hbox), avatar_widget(acc, nick, color, 40), FALSE, FALSE, 0);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *l1 = gtk_label_new(NULL);
    GtkWidget *l2 = gtk_label_new(NULL);
    char buf[256];
    if (kind == 0)
        snprintf(buf, sizeof(buf),
            "<span weight='bold'>%s</span> <span color='#94a3b8' size='small'>(%s)</span>", nick, acc);
    else
        snprintf(buf, sizeof(buf),
            "<span weight='bold'>%s</span> <span color='#94a3b8' size='small'>申请加入 %s</span>", nick, gname);
    gtk_label_set_markup(GTK_LABEL(l1), buf);
    gtk_label_set_xalign(GTK_LABEL(l1), 0.0);
    snprintf(buf, sizeof(buf), "<span color='#475569' size='small'>“%s”</span>",
             hello && *hello ? hello : "(无附言)");
    gtk_label_set_markup(GTK_LABEL(l2), buf);
    gtk_label_set_xalign(GTK_LABEL(l2), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), l1, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), l2, FALSE, FALSE, 0);
    gtk_widget_set_hexpand(vbox, TRUE);
    gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);

    GtkWidget *by = gtk_button_new_with_label("同意");
    GtkWidget *bn = gtk_button_new_with_label("拒绝");
    gtk_style_context_add_class(gtk_widget_get_style_context(by), "im-primary");
    gtk_style_context_add_class(gtk_widget_get_style_context(bn), "im-secondary");
    gtk_box_pack_start(GTK_BOX(hbox), by, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), bn, FALSE, FALSE, 0);

    RowData *rd = g_malloc0(sizeof(*rd));
    rd->kind = kind; rd->reqid = reqid; rd->gid = gid;
    g_object_set_data_full(G_OBJECT(row), "rd", rd, (GDestroyNotify)row_data_free);
    g_signal_connect(by, "clicked", G_CALLBACK(on_req_accept), rd);
    g_signal_connect(bn, "clicked", G_CALLBACK(on_req_reject), rd);

    gtk_widget_show_all(row);
    return row;
}

/* ===================== 主窗 ===================== */
static void on_friend_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer ud);
static void on_group_row_selected (GtkListBox *box, GtkListBoxRow *row, gpointer ud);
static gboolean on_listbox_button(GtkWidget *w, GdkEventButton *ev, gpointer ud);

void show_main(void) {
    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    char title[128];
    snprintf(title, sizeof(title), "chat_linux - %s (%s)", CTX.nickname, CTX.account);
    gtk_window_set_title(GTK_WINDOW(w), title);
    gtk_window_set_default_size(GTK_WINDOW(w), 960, 620);
    gtk_window_set_position(GTK_WINDOW(w), GTK_WIN_POS_CENTER);
    g_signal_connect(w, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    CTX.main_win = w;

    GtkWidget *hpane = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_container_add(GTK_CONTAINER(w), hpane);

    /* ============ 左侧栏 ============ */
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(left), "im-sidebar");

    /* 自己的头像/名片 */
    GtkWidget *self = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(self), "im-self");
    CTX.self_avatar = avatar_widget(CTX.account, CTX.nickname, CTX.avatar_color, 48);
    gtk_box_pack_start(GTK_BOX(self), CTX.self_avatar, FALSE, FALSE, 0);
    GtkWidget *svb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    char buf[128];
    CTX.self_nick_lbl = gtk_label_new(NULL);
    snprintf(buf, sizeof(buf), "<span size='medium' weight='bold'>%s</span>", CTX.nickname);
    gtk_label_set_markup(GTK_LABEL(CTX.self_nick_lbl), buf);
    gtk_label_set_xalign(GTK_LABEL(CTX.self_nick_lbl), 0.0);
    CTX.self_acc_lbl = gtk_label_new(NULL);
    snprintf(buf, sizeof(buf), "<span color='#94a3b8' size='small'>账号 %s</span>", CTX.account);
    gtk_label_set_markup(GTK_LABEL(CTX.self_acc_lbl), buf);
    gtk_label_set_xalign(GTK_LABEL(CTX.self_acc_lbl), 0.0);
    gtk_box_pack_start(GTK_BOX(svb), CTX.self_nick_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(svb), CTX.self_acc_lbl,  FALSE, FALSE, 0);
    gtk_widget_set_hexpand(svb, TRUE);
    gtk_box_pack_start(GTK_BOX(self), svb, TRUE, TRUE, 0);

    /* === "+" 添加按钮 ===
     * 原本放在右上 header 上, 用户反馈不顺手, 这里挪到自己头像卡的右侧.
     * 蓝色实心圆 + 白色"＋", 在白底自卡片上视觉权重正好. */
    CTX.add_btn = gtk_button_new_with_label("＋");
    gtk_style_context_add_class(gtk_widget_get_style_context(CTX.add_btn), "im-addbtn-self");
    gtk_widget_set_size_request(CTX.add_btn, 36, 36);
    gtk_widget_set_valign(CTX.add_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(CTX.add_btn, "添加好友 / 加群 / 建群");
    gtk_box_pack_end(GTK_BOX(self), CTX.add_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(left), self, FALSE, FALSE, 0);

    /* 三个 tab */
    GtkWidget *nb = gtk_notebook_new();

    CTX.friend_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(CTX.friend_box), GTK_SELECTION_SINGLE);
    g_signal_connect(CTX.friend_box, "row-selected", G_CALLBACK(on_friend_row_selected), NULL);
    g_signal_connect(CTX.friend_box, "button-press-event", G_CALLBACK(on_listbox_button), NULL);
    GtkWidget *fs = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(fs), CTX.friend_box);
    gtk_widget_set_size_request(fs, 280, 360);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), fs, gtk_label_new("好友"));

    CTX.group_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(CTX.group_box), GTK_SELECTION_SINGLE);
    g_signal_connect(CTX.group_box, "row-selected", G_CALLBACK(on_group_row_selected), NULL);
    g_signal_connect(CTX.group_box, "button-press-event", G_CALLBACK(on_listbox_button), NULL);
    GtkWidget *gs = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(gs), CTX.group_box);
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), gs, gtk_label_new("群组"));

    CTX.req_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(CTX.req_box), GTK_SELECTION_NONE);
    GtkWidget *rs = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(rs), CTX.req_box);
    CTX.req_count_lbl = gtk_label_new("通知");
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), rs, CTX.req_count_lbl);

    gtk_box_pack_start(GTK_BOX(left), nb, TRUE, TRUE, 0);
    gtk_paned_add1(GTK_PANED(hpane), left);

    /* ============ 右侧聊天面板 ============ */
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(hdr), "im-header");
    CTX.chat_avatar_area = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(hdr), CTX.chat_avatar_area, FALSE, FALSE, 0);
    CTX.chat_header = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(CTX.chat_header),
        "<span color='white'>请在左侧选择好友或群开始聊天</span>");
    gtk_label_set_xalign(GTK_LABEL(CTX.chat_header), 0.0);
    gtk_widget_set_hexpand(CTX.chat_header, TRUE);
    gtk_box_pack_start(GTK_BOX(hdr), CTX.chat_header, TRUE, TRUE, 0);
    /* + 按钮已经挪到左侧自己头像旁边了, 这里不再创建. */
    gtk_box_pack_start(GTK_BOX(right), hdr, FALSE, FALSE, 0);

    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin (GTK_TEXT_VIEW(tv), 10);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(tv), 10);
    gtk_text_view_set_top_margin  (GTK_TEXT_VIEW(tv), 10);
    CTX.chat_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    GtkWidget *sv = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sv), tv);
    gtk_box_pack_start(GTK_BOX(right), sv, TRUE, TRUE, 0);

    GtkWidget *ibox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(ibox), 8);
    CTX.input_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(CTX.input_entry), "输入消息, 回车发送");
    GtkWidget *bfile = gtk_button_new_with_label("📎");
    GtkWidget *bsnd  = gtk_button_new_with_label("发送");
    gtk_style_context_add_class(gtk_widget_get_style_context(bfile), "im-secondary");
    gtk_style_context_add_class(gtk_widget_get_style_context(bsnd),  "im-primary");
    gtk_widget_set_size_request(bsnd,  88, -1);
    gtk_widget_set_size_request(bfile, 44, -1);
    gtk_box_pack_start(GTK_BOX(ibox), CTX.input_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(ibox), bfile, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ibox), bsnd,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right), ibox, FALSE, FALSE, 0);

    g_signal_connect(bsnd,            "clicked",  G_CALLBACK(on_send_clicked), NULL);
    g_signal_connect(CTX.input_entry, "activate", G_CALLBACK(on_send_clicked), NULL);
    g_signal_connect(bfile,           "clicked",  G_CALLBACK(on_sendfile),     NULL);
    g_signal_connect(CTX.add_btn,     "clicked",  G_CALLBACK(on_add_clicked),  NULL);

    gtk_paned_add2(GTK_PANED(hpane), right);
    gtk_paned_set_position(GTK_PANED(hpane), 300);

    gtk_widget_show_all(w);

    Message m;
    memset(&m, 0, sizeof(m)); m.type = MSG_FRIEND_LIST;       net_send(&m);
    memset(&m, 0, sizeof(m)); m.type = MSG_GROUP_LIST;        net_send(&m);
    memset(&m, 0, sizeof(m)); m.type = MSG_FRIEND_REQ_LIST;   net_send(&m);
    memset(&m, 0, sizeof(m)); m.type = MSG_GROUP_JOIN_REQ_LIST; net_send(&m);
}

/* ===================== 行选择 → 切会话 ===================== */
static void switch_chat_target(int is_group, const char *acc_or_gname,
                               const char *nick, int color, int gid) {
    CTX.peer_is_group = is_group;
    if (is_group) {
        CTX.peer_group_id = gid;
        strncpy(CTX.peer_name, acc_or_gname, MAX_NAME_LEN - 1);
        strncpy(CTX.peer_nick, acc_or_gname, MAX_NAME_LEN - 1);
    } else {
        CTX.peer_group_id = 0;
        strncpy(CTX.peer_name, acc_or_gname, MAX_NAME_LEN - 1);
        strncpy(CTX.peer_nick, nick, MAX_NAME_LEN - 1);
    }
    CTX.peer_color = color;

    /* 更新 header: 头像 + 标题 */
    GList *kids = gtk_container_get_children(GTK_CONTAINER(CTX.chat_avatar_area));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);
    gtk_box_pack_start(GTK_BOX(CTX.chat_avatar_area),
                       avatar_widget(is_group ? NULL : acc_or_gname,
                                     nick ? nick : acc_or_gname, color, 36),
                       FALSE, FALSE, 0);
    gtk_widget_show_all(CTX.chat_avatar_area);

    char hdr[256];
    if (is_group)
        snprintf(hdr, sizeof(hdr),
            "<span color='white' weight='bold' size='large'>%s</span>"
            "  <span color='#cfeefd' size='small'>群号 %d</span>",
            acc_or_gname, gid);
    else
        snprintf(hdr, sizeof(hdr),
            "<span color='white' weight='bold' size='large'>%s</span>"
            "  <span color='#cfeefd' size='small'>%s</span>",
            nick, acc_or_gname);
    gtk_label_set_markup(GTK_LABEL(CTX.chat_header), hdr);

    gtk_text_buffer_set_text(CTX.chat_buf, "", -1);
    Message hm; memset(&hm, 0, sizeof(hm));
    if (is_group) {
        hm.type = MSG_HISTORY_GROUP; hm.group_id = gid;
    } else {
        hm.type = MSG_HISTORY_PRIV;
        strncpy(hm.to_name, acc_or_gname, MAX_NAME_LEN - 1);
    }
    net_send(&hm);
}

static void on_friend_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer ud) {
    (void)box; (void)ud;
    if (!row) return;
    RowData *rd = g_object_get_data(G_OBJECT(row), "rd");
    if (!rd) return;
    switch_chat_target(0, rd->acc, rd->nick, rd->color, 0);
}

static void on_group_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer ud) {
    (void)box; (void)ud;
    if (!row) return;
    RowData *rd = g_object_get_data(G_OBJECT(row), "rd");
    if (!rd) return;
    switch_chat_target(1, rd->gname, rd->gname, rd->gid % 10, rd->gid);
}

/* ===================== 右键菜单 (好友/群) ===================== */
static void menu_history(GtkMenuItem *mi, gpointer ud) {
    RowData *rd = ud;
    gtk_text_buffer_set_text(CTX.chat_buf, "", -1);
    if (rd->kind == -2) {
        Message hm; memset(&hm, 0, sizeof(hm));
        hm.type = MSG_HISTORY_GROUP; hm.group_id = rd->gid;
        net_send(&hm);
    } else {
        net_send_text(MSG_HISTORY_PRIV, rd->acc, 0, NULL);
    }
}
static void menu_del(GtkMenuItem *mi, gpointer ud) {
    RowData *rd = ud;
    net_send_text(MSG_FRIEND_DEL, rd->acc, 0, NULL);
    net_send_text(MSG_FRIEND_LIST, NULL, 0, NULL);
}
static void menu_black_toggle(GtkMenuItem *mi, gpointer ud) {
    RowData *rd = ud;
    int is_black = rd->reqid;
    net_send_text(is_black ? MSG_BLACK_DEL : MSG_BLACK_ADD, rd->acc, 0, NULL);
    net_send_text(MSG_FRIEND_LIST, NULL, 0, NULL);
}

static gboolean on_listbox_button(GtkWidget *w, GdkEventButton *ev, gpointer ud) {
    (void)ud;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3) return FALSE;
    GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(w), ev->y);
    if (!row) return FALSE;
    gtk_list_box_select_row(GTK_LIST_BOX(w), row);
    RowData *rd = g_object_get_data(G_OBJECT(row), "rd");
    if (!rd) return FALSE;
    GtkWidget *menu = gtk_menu_new();
    if (rd->kind == -1) {  /* 好友 */
        GtkWidget *mi1 = gtk_menu_item_new_with_label("查看历史");
        GtkWidget *mi2 = gtk_menu_item_new_with_label(rd->reqid ? "移出黑名单" : "加入黑名单");
        GtkWidget *mi3 = gtk_menu_item_new_with_label("删除好友");
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi1);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi2);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi3);
        g_signal_connect(mi1, "activate", G_CALLBACK(menu_history),      rd);
        g_signal_connect(mi2, "activate", G_CALLBACK(menu_black_toggle), rd);
        g_signal_connect(mi3, "activate", G_CALLBACK(menu_del),          rd);
    } else {                /* 群 */
        GtkWidget *mi1 = gtk_menu_item_new_with_label("查看历史");
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi1);
        g_signal_connect(mi1, "activate", G_CALLBACK(menu_history), rd);
    }
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

/* ===================== 申请项: 同意/拒绝 ===================== */
static void on_req_accept(GtkButton *b, gpointer ud) {
    (void)b;
    RowData *rd = ud;
    Message m; memset(&m, 0, sizeof(m));
    m.type     = rd->kind == 0 ? MSG_FRIEND_REQ_REPLY : MSG_GROUP_JOIN_REPLY;
    m.status   = rd->reqid;
    m.group_id = 1;
    net_send(&m);
    Message q; memset(&q, 0, sizeof(q));
    q.type = (rd->kind == 0) ? MSG_FRIEND_REQ_LIST : MSG_GROUP_JOIN_REQ_LIST; net_send(&q);
    memset(&q, 0, sizeof(q)); q.type = MSG_FRIEND_LIST; net_send(&q);
    memset(&q, 0, sizeof(q)); q.type = MSG_GROUP_LIST;  net_send(&q);
}
static void on_req_reject(GtkButton *b, gpointer ud) {
    (void)b;
    RowData *rd = ud;
    Message m; memset(&m, 0, sizeof(m));
    m.type     = rd->kind == 0 ? MSG_FRIEND_REQ_REPLY : MSG_GROUP_JOIN_REPLY;
    m.status   = rd->reqid;
    m.group_id = 0;
    net_send(&m);
    Message q; memset(&q, 0, sizeof(q));
    q.type = (rd->kind == 0) ? MSG_FRIEND_REQ_LIST : MSG_GROUP_JOIN_REQ_LIST; net_send(&q);
}

/* ===================== 发送/文件 ===================== */
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
    char me[64]; snprintf(me, sizeof(me), "%s", CTX.nickname);
    ui_append_chat(me, ts, text);
    gtk_entry_set_text(GTK_ENTRY(CTX.input_entry), "");
}

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

/* ===================== "+" 菜单 ===================== */
static void plus_addfriend(GtkMenuItem *mi, gpointer ud) { (void)mi;(void)ud; open_search_dialog(1); }
static void plus_addgroup (GtkMenuItem *mi, gpointer ud) { (void)mi;(void)ud; open_search_dialog(0); }
static void plus_creategroup(GtkMenuItem *mi, gpointer ud) { (void)mi;(void)ud; open_create_group_dialog(); }

static void on_add_clicked(GtkButton *b, gpointer ud) {
    (void)ud;
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
    GtkWidget    *listbox;
} SearchDlg;

static SearchDlg *g_search_dlg = NULL;

static void clear_listbox(GtkWidget *lb) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(lb));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);
}

void ui_search_result(int is_user, const char *body) {
    if (!g_search_dlg || g_search_dlg->is_user != is_user) return;
    clear_listbox(g_search_dlg->listbox);
    if (!body || !*body) return;
    char *dup = g_strdup(body);
    char *save = NULL, *line = strtok_r(dup, "\n", &save);
    while (line) {
        if (is_user) {
            /* "account\tnickname\tcolor\tonline" */
            char *t1 = strchr(line, '\t');           if (!t1) goto next;
            *t1 = 0;
            char *t2 = strchr(t1+1, '\t');           if (!t2) goto next;
            *t2 = 0;
            char *t3 = strchr(t2+1, '\t');           if (!t3) goto next;
            *t3 = 0;
            int color  = atoi(t2+1);
            int online = atoi(t3+1);
            GtkWidget *row = make_friend_row(line, t1+1, color, online, 0);
            gtk_container_add(GTK_CONTAINER(g_search_dlg->listbox), row);
        } else {
            /* "gid\tname\towner\tcnt" */
            char *t1 = strchr(line, '\t');           if (!t1) goto next;
            *t1 = 0; int gid = atoi(line);
            char *t2 = strchr(t1+1, '\t');           if (!t2) goto next;
            *t2 = 0;
            GtkWidget *row = make_group_row(gid, t1+1);
            gtk_container_add(GTK_CONTAINER(g_search_dlg->listbox), row);
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
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(sd->listbox));
    if (!row) {
        msgbox(GTK_WINDOW(sd->dlg), GTK_MESSAGE_WARNING, "请先选择一个目标"); return;
    }
    RowData *rd = g_object_get_data(G_OBJECT(row), "rd");
    if (!rd) return;
    char hello[256] = {0};
    prompt_text(sd->is_user ? "好友申请" : "入群申请", "附言 (可留空):", hello, sizeof(hello));
    Message req; memset(&req, 0, sizeof(req));
    if (sd->is_user) {
        req.type = MSG_FRIEND_REQ;
        strncpy(req.to_name, rd->acc, MAX_NAME_LEN - 1);
    } else {
        req.type = MSG_GROUP_JOIN_REQ;
        req.group_id = rd->gid;
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
    gtk_window_set_default_size(GTK_WINDOW(d), 480, 460);
    gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(CTX.main_win));
    gtk_window_set_modal(GTK_WINDOW(d), TRUE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
    gtk_container_add(GTK_CONTAINER(d), vbox);

    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *en = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(en),
        is_user ? "输入账号 (6 位数字) 或 昵称关键字" : "输入群名关键字");
    gtk_widget_set_hexpand(en, TRUE);
    GtkWidget *bs = gtk_button_new_with_label("搜索");
    gtk_style_context_add_class(gtk_widget_get_style_context(bs), "im-secondary");
    gtk_box_pack_start(GTK_BOX(hb), en, TRUE,  TRUE,  0);
    gtk_box_pack_start(GTK_BOX(hb), bs, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), hb, FALSE, FALSE, 0);

    GtkWidget *lb = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(lb), GTK_SELECTION_SINGLE);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), lb);
    gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

    GtkWidget *btn = gtk_button_new_with_label(is_user ? "发送好友申请" : "申请加入");
    gtk_style_context_add_class(gtk_widget_get_style_context(btn), "im-primary");
    gtk_widget_set_size_request(btn, -1, 42);
    gtk_box_pack_start(GTK_BOX(vbox), btn, FALSE, FALSE, 0);

    SearchDlg *sd = g_malloc0(sizeof(*sd));
    sd->is_user = is_user; sd->dlg = d; sd->entry = en; sd->listbox = lb;
    g_search_dlg = sd;

    g_signal_connect(bs,  "clicked",  G_CALLBACK(search_do),    sd);
    g_signal_connect(en,  "activate", G_CALLBACK(search_do),    sd);
    g_signal_connect(btn, "clicked",  G_CALLBACK(search_apply), sd);
    g_signal_connect(d,   "destroy",  G_CALLBACK(search_destroy), NULL);
    gtk_widget_show_all(d);
}

static void open_create_group_dialog(void) {
    char name[64] = {0};
    if (!prompt_text("创建群", "请输入群名:", name, sizeof(name))) return;
    Message m; memset(&m, 0, sizeof(m));
    m.type = MSG_GROUP_CREATE;
    strncpy(m.body, name, MAX_BODY_LEN - 1); m.body_len = strlen(name);
    net_send(&m);
    net_send_text(MSG_GROUP_LIST, NULL, 0, NULL);
}

/* ===================== 接收线程回调 ===================== */
void ui_append_chat(const char *who, const char *time, const char *text) {
    GtkTextIter it;
    gtk_text_buffer_get_end_iter(CTX.chat_buf, &it);
    char line[MAX_BODY_LEN + 128];
    snprintf(line, sizeof(line), "[%s] %s: %s\n",
             time ? time : "", who ? who : "?", text ? text : "");
    gtk_text_buffer_insert(CTX.chat_buf, &it, line, -1);
}

void ui_refresh_friends(const char *body) {
    clear_listbox(CTX.friend_box);
    if (!body || !*body) return;
    char *dup = g_strdup(body);
    char *save = NULL, *line = strtok_r(dup, "\n", &save);
    while (line) {
        /* "account\tnickname\tcolor\tonline\tblack" */
        char *t1 = strchr(line, '\t');     if (!t1) goto next;
        *t1 = 0;
        char *t2 = strchr(t1+1, '\t');     if (!t2) goto next;
        *t2 = 0;
        char *t3 = strchr(t2+1, '\t');     if (!t3) goto next;
        *t3 = 0;
        char *t4 = strchr(t3+1, '\t');     if (!t4) goto next;
        *t4 = 0;
        int color  = atoi(t2+1);
        int online = atoi(t3+1);
        int black  = atoi(t4+1);
        gtk_container_add(GTK_CONTAINER(CTX.friend_box),
                          make_friend_row(line, t1+1, color, online, black));
        next:
        line = strtok_r(NULL, "\n", &save);
    }
    g_free(dup);
    gtk_widget_show_all(CTX.friend_box);
}

void ui_refresh_groups(const char *body) {
    clear_listbox(CTX.group_box);
    if (!body || !*body) return;
    char *dup = g_strdup(body);
    char *save = NULL, *line = strtok_r(dup, "\n", &save);
    while (line) {
        char *t1 = strchr(line, '\t');     if (!t1) goto next;
        *t1 = 0; int gid = atoi(line);
        char *t2 = strchr(t1+1, '\t');     if (t2) *t2 = 0;
        gtk_container_add(GTK_CONTAINER(CTX.group_box),
                          make_group_row(gid, t1+1));
        next:
        line = strtok_r(NULL, "\n", &save);
    }
    g_free(dup);
    gtk_widget_show_all(CTX.group_box);
}

/* req_box 同时显示好友/入群申请. body 是某一类的完整列表,
 * 我们先清掉同 kind 项, 再插入新项. */
static void clear_kind(int kind) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(CTX.req_box));
    for (GList *l = kids; l; l = l->next) {
        RowData *rd = g_object_get_data(G_OBJECT(l->data), "rd");
        if (rd && rd->kind == kind) gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(kids);
}

static void refresh_req_tab(void) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(CTX.req_box));
    int n = g_list_length(kids);
    g_list_free(kids);
    char buf[32];
    if (n > 0) snprintf(buf, sizeof(buf), "通知 (%d)", n);
    else       snprintf(buf, sizeof(buf), "通知");
    gtk_label_set_text(GTK_LABEL(CTX.req_count_lbl), buf);
}

void ui_refresh_requests(int kind, const char *body) {
    clear_kind(kind);
    if (!body || !*body) { refresh_req_tab(); return; }
    char *dup = g_strdup(body);
    char *save = NULL, *line = strtok_r(dup, "\n", &save);
    while (line) {
        char *f[7] = {0};
        int n = 0;
        char *p = line, *q;
        while (n < 7 && (q = strchr(p, '\t'))) { *q = 0; f[n++] = p; p = q + 1; }
        if (*p && n < 7) f[n++] = p;
        if (kind == 0) {
            /* "reqid\tfrom_acc\tfrom_nick\tcolor\ttime\thello" */
            if (n >= 5) {
                int reqid = atoi(f[0]);
                int color = atoi(f[3]);
                gtk_container_add(GTK_CONTAINER(CTX.req_box),
                    make_req_row(0, reqid, f[1], f[2], color, NULL, f[5] ? f[5] : "", 0));
            }
        } else {
            /* "reqid\tgid\tgname\tfrom_nick\ttime\thello" */
            if (n >= 5) {
                int reqid = atoi(f[0]);
                int gid   = atoi(f[1]);
                gtk_container_add(GTK_CONTAINER(CTX.req_box),
                    make_req_row(1, reqid, "", f[3], gid % 10, f[2], f[5] ? f[5] : "", gid));
            }
        }
        line = strtok_r(NULL, "\n", &save);
    }
    g_free(dup);
    gtk_widget_show_all(CTX.req_box);
    refresh_req_tab();
}

void ui_add_request(int kind, int reqid, const char *acc, const char *nick, int color,
                    const char *gname, const char *hello, int gid) {
    /* 去重 */
    GList *kids = gtk_container_get_children(GTK_CONTAINER(CTX.req_box));
    for (GList *l = kids; l; l = l->next) {
        RowData *rd = g_object_get_data(G_OBJECT(l->data), "rd");
        if (rd && rd->kind == kind && rd->reqid == reqid) { g_list_free(kids); return; }
    }
    g_list_free(kids);
    gtk_container_add(GTK_CONTAINER(CTX.req_box),
        make_req_row(kind, reqid, acc, nick, color, gname, hello, gid));
    gtk_widget_show_all(CTX.req_box);
    refresh_req_tab();
}

void ui_notify_text(const char *title, const char *text) {
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(CTX.main_win),
        GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", title ? title : "");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", text ? text : "");
    g_signal_connect(d, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show_all(d);
}
