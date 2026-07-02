/* =========================================================
 *  zoo 桌面客户端 (C + WebKitGTK)
 *
 *  设计:
 *   - 用 GTK3 创建一个无边框窗口, 内嵌 WebKitWebView 渲染界面;
 *     界面仍是原来那套 prototype/zoo-chat.html(+zoo-net.js), UI 完全不变。
 *   - 主体(窗口宿主 + 网络通信 + 协议编解码) 全部用 C 实现:
 *       * 通过自定义 URI scheme "app://zoo/..." 把 prototype 目录当作站点
 *         提供给 WebView(有稳定 origin, localStorage 等可用);
 *       * 注入一段 shim, 在页面里重建 window.zooNative(connect/send/onMsg/win),
 *         这样 zoo-net.js / zoo-chat.html 一行都不用改;
 *       * JS -> C: 页面调用 window.webkit.messageHandlers.zoo.postMessage(JSON);
 *       * C  -> JS: 在主线程 run_javascript(window.__zooDeliver("<base64-json>"));
 *       * 真正的 TCP 连接 / 4240B 定长 Message 收发 / JSON<->Message 编解码
 *         由 C 完成(复用 common/protocol.h + common/net_io.c)。
 *
 *  环境变量:
 *     CHAT_PORT       服务器端口(默认 8888)
 *     ZOO_PROTO_DIR   prototype 目录(默认按可执行文件位置自动推断)
 *     ZOO_SELFTEST    置 1 时启动后自动跑一次注册/登录自检, 打印结果并退出
 * ========================================================= */
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include "protocol.h"
#include "net_io.h"

/* ---------- 全局状态 ---------- */
static GtkWidget       *g_win = NULL;
static WebKitWebView   *g_web = NULL;
static int              g_fd  = -1;                 /* 当前连接 socket */
static pthread_mutex_t  g_fd_mu = PTHREAD_MUTEX_INITIALIZER;
static volatile guint   g_gen = 0;                  /* 连接代号: 重连即自增 */
static int              g_port = SERVER_PORT;
static char             g_proto_dir[4096] = "prototype";
static char             g_shot[4096] = "";          /* ZOO_SHOT: 渲染后截图存盘并退出 */
static guint            g_shot_delay = 2500;         /* ZOO_SHOT_DELAY: 截图前等待 ms */

/* 注入页面的 shim: 在页面里重建 window.zooNative, 让前端无感知 */
static const char *SHIM_JS =
"(function(){"
"  if (window.zooNative) return;"
"  var mh=(window.webkit&&window.webkit.messageHandlers)?window.webkit.messageHandlers.zoo:null;"
"  var cb=null, dec=new TextDecoder('utf-8');"
"  function b2u(b){var s=atob(b),n=s.length,u=new Uint8Array(n);for(var i=0;i<n;i++)u[i]=s.charCodeAt(i);return u;}"
"  window.__zooDeliver=function(b64){try{var o=JSON.parse(dec.decode(b2u(b64)));"
"    if(o.bodyB64!=null){o.body=dec.decode(b2u(o.bodyB64));}else if(o.body==null){o.body='';}"
"    if(cb)cb(o);}catch(e){console.error('zooDeliver',e);}};"
"  window.zooNative={"
"    connect:function(host){if(mh)mh.postMessage(JSON.stringify({cmd:'connect',host:host||''}));return Promise.resolve(true);},"
"    send:function(obj){if(mh)mh.postMessage(JSON.stringify({cmd:'send',msg:obj}));},"
"    onMsg:function(fn){cb=fn;},"
"    win:{minimize:function(){if(mh)mh.postMessage(JSON.stringify({cmd:'win',op:'min'}));},"
"         toggleMaximize:function(){if(mh)mh.postMessage(JSON.stringify({cmd:'win',op:'max'}));},"
"         close:function(){if(mh)mh.postMessage(JSON.stringify({cmd:'win',op:'close'}));}}"
"  };"
"  document.addEventListener('mousedown',function(e){"
"    if(e.button!==0||!mh)return;var node=e.target;"
"    while(node){if(node.classList){"
"      if(node.classList.contains('tb-win')||node.classList.contains('tb-dots')||node.classList.contains('tb-logo')||node.classList.contains('net-status'))return;"
"      if(node.classList.contains('titlebar')){mh.postMessage(JSON.stringify({cmd:'drag',x:e.screenX,y:e.screenY}));return;}"
"    }node=node.parentNode;}"
"  },true);"
"})();";

/* 自检脚本(仅 ZOO_SELFTEST=1 时注入): 单连接跑通注册/登录/私聊/历史/建群/群聊/资料,
 * 重点验证 C 的 JSON<->Message 编解码(含中文+emoji 双向 UTF-8 经 base64 往返)。 */
static const char *SELFTEST_JS =
"(function(){"
"  function rep(ok,info){window.webkit.messageHandlers.zoo.postMessage(JSON.stringify({cmd:'report',ok:ok,info:String(info)}));}"
"  function wait(ms){return new Promise(function(r){setTimeout(r,ms);});}"
"  var steps=[];"
"  function ck(name,cond){steps.push((cond?'+':'-')+name);if(!cond)throw new Error('FAIL@'+name);}"
"  (async function(){try{"
"    await wait(1500);"
"    ck('net',typeof net!=='undefined'&&!!net);"
"    try{await net.connect('127.0.0.1');}catch(e){throw new Error('connect '+e.message);}"
"    var ts=Date.now(),nick='t'+ts;"
"    var r=await net.register(nick,'pass123',nick+'@x.com');ck('register',r.status===0);"
"    var acc=(r.body||'').trim();ck('acc',/^[0-9]{6,}$/.test(acc));"
"    var lr=await net.login(acc,'pass123');ck('login',lr.status===0);"
"    await wait(600);"
"    var realf=(friends||[]).filter(function(f){return /^[0-9]{6,}$/.test(f.id);});"
"    ck('friendlist',realf.length>0);"            /* 注册自动送的小助手/新手指南 */
"    var fid=realf[0].id;"
"    window.__pm=[];net.on(ZooNet.T.PRIVATE_CHAT,function(m){window.__pm.push(m);});"
"    var TEXT='\\u4f60\\u597d\\ud83d\\udc2f C-WebView '+ts;"        /* 你好🐯 */
"    net.sendPrivate(fid,TEXT);await wait(600);"
"    net.historyPriv(fid);await wait(1200);"
"    var bodies=window.__pm.map(function(m){return m.body;});"
"    window.__dbg='fid='+fid+' pmN='+bodies.length+' bodies='+JSON.stringify(bodies);"
"    ck('priv-utf8-roundtrip',bodies.indexOf(TEXT)>=0);"
"    var gr=await net.groupCreate('grp'+ts);ck('groupcreate',gr.status===0);"
"    var gid=(gr.body||'').trim();net.sendGroup(gid,'\\u7fa4\\u6d88\\u606f'+ts);"   /* 群消息 */
"    window.__pd=null;net.on(ZooNet.T.PROFILE_DATA,function(m){window.__pd=ZooNet.parseProfile(m.body);});"
"    var NN='\\u731b\\u517d'+(ts%1000);"                            /* 猛兽 */
"    var ps=await net.profileSet(NN,'2001-02-03');ck('profileset',ps.status===0);"
"    net.profileGet(acc);await wait(500);"
"    ck('profileget-utf8',!!window.__pd&&window.__pd.nick===NN);"
"    rep(true,steps.join(' ')+' acc='+acc+' gid='+gid);"
"  }catch(e){rep(false,steps.join(' ')+' :: '+e.message+' | '+(window.__dbg||''));}})();"
"})();";

/* 自动登录脚本(仅 ZOO_AUTOLOGIN=1, 截图用): 注册一个账号并走真实 doLogin 进入主界面 */
static const char *AUTOLOGIN_JS =
"(function(){function wait(ms){return new Promise(function(r){setTimeout(r,ms);});}"
"(async function(){try{await wait(1600);if(typeof net==='undefined'||!net)return;"
"  try{await net.connect('127.0.0.1');}catch(e){}"
"  var nick='demo'+Date.now();var r=await net.register(nick,'pass123',nick+'@x.com');"
"  var acc=(r.body||'').trim();"
"  document.getElementById('srvHost').value='127.0.0.1';"
"  document.getElementById('lgEmail').value=acc;"
"  document.getElementById('lgPwd').value='pass123';"
"  doLogin();"
"}catch(e){console.error('autologin',e);}})();})();";

/* ---------- 小工具 ---------- */
static const char *json_str(cJSON *o, const char *k){
    cJSON *i = cJSON_GetObjectItem(o, k);
    return (i && cJSON_IsString(i)) ? i->valuestring : NULL;
}
static double json_num(cJSON *o, const char *k){
    cJSON *i = cJSON_GetObjectItem(o, k);
    return (i && cJSON_IsNumber(i)) ? i->valuedouble : 0;
}
/* 定长 char[32] 字段 -> cJSON 字符串(保证有效 UTF-8) */
static void add_field(cJSON *o, const char *key, const char *src){
    char buf[MAX_NAME_LEN + 1];
    memcpy(buf, src, MAX_NAME_LEN);
    buf[MAX_NAME_LEN] = 0;
    char *valid = g_utf8_make_valid(buf, -1);
    cJSON_AddStringToObject(o, key, valid ? valid : "");
    g_free(valid);
}
/* cJSON 字符串 -> 定长 char[32] 字段 */
static void copy_field(char *dst, const char *src){
    memset(dst, 0, MAX_NAME_LEN);
    if (src) { strncpy(dst, src, MAX_NAME_LEN - 1); }
}

/* ---------- C -> JS 投递(必须在 GTK 主线程执行) ---------- */
static gboolean deliver_idle(gpointer data){
    char *b64 = (char *)data;
    if (g_web){
        char *js = g_strdup_printf("window.__zooDeliver(\"%s\")", b64);
        webkit_web_view_run_javascript(g_web, js, NULL, NULL, NULL);
        g_free(js);
    }
    g_free(b64);
    return G_SOURCE_REMOVE;
}
static void deliver_json(const char *json){
    char *b64 = g_base64_encode((const guchar *)json, strlen(json));
    g_idle_add(deliver_idle, b64);   /* 跨线程安全: 交回主线程 */
}
static void deliver_bridge(gboolean ok, const char *msg){
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "type", 0);
    cJSON_AddStringToObject(o, "_ev", "bridge");
    cJSON_AddBoolToObject(o, "ok", ok);
    cJSON_AddStringToObject(o, "msg", msg ? msg : "");
    char *json = cJSON_PrintUnformatted(o);
    deliver_json(json);
    cJSON_free(json);
    cJSON_Delete(o);
}
static void deliver_msg(const Message *m){
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "type",     m->type);
    cJSON_AddNumberToObject(o, "status",   m->status);
    cJSON_AddNumberToObject(o, "group_id", m->group_id);
    cJSON_AddNumberToObject(o, "body_len", m->body_len);
    add_field(o, "from_name", m->from_name);
    add_field(o, "to_name",   m->to_name);
    add_field(o, "from_nick", m->from_nick);
    add_field(o, "timestamp", m->timestamp);
    guint blen = m->body_len; if (blen > MAX_BODY_LEN) blen = MAX_BODY_LEN;
    char *bb = g_base64_encode((const guchar *)m->body, blen);   /* body 走 base64, 不怕二进制/截断 */
    cJSON_AddStringToObject(o, "bodyB64", bb);
    g_free(bb);
    char *json = cJSON_PrintUnformatted(o);
    deliver_json(json);
    cJSON_free(json);
    cJSON_Delete(o);
}

/* ---------- TCP 接收线程 ---------- */
typedef struct { char host[256]; int port; guint gen; } ConnCtx;

static void *reader_thread(void *arg){
    ConnCtx *c = (ConnCtx *)arg;
    guint mygen = c->gen;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    char portstr[16]; snprintf(portstr, sizeof(portstr), "%d", c->port);
    if (getaddrinfo(c->host, portstr, &hints, &res) != 0 || !res){
        deliver_bridge(FALSE, "无法解析服务器地址"); free(c); return NULL;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0){ freeaddrinfo(res); deliver_bridge(FALSE, "socket 创建失败"); free(c); return NULL; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0){
        close(fd); freeaddrinfo(res); deliver_bridge(FALSE, "无法连接服务器"); free(c); return NULL;
    }
    freeaddrinfo(res);

    pthread_mutex_lock(&g_fd_mu);
    if (mygen != g_gen){ pthread_mutex_unlock(&g_fd_mu); close(fd); free(c); return NULL; }
    g_fd = fd;
    pthread_mutex_unlock(&g_fd_mu);

    deliver_bridge(TRUE, "connected");

    Message m;
    while (mygen == g_gen && recv_msg(fd, &m) == 0){
        deliver_msg(&m);
    }

    pthread_mutex_lock(&g_fd_mu);
    if (g_fd == fd) g_fd = -1;
    pthread_mutex_unlock(&g_fd_mu);
    close(fd);
    if (mygen == g_gen) deliver_bridge(FALSE, "connection closed");
    free(c);
    return NULL;
}

static void start_connect(const char *host){
    pthread_mutex_lock(&g_fd_mu);
    g_gen++; guint gen = g_gen;
    int old = g_fd; g_fd = -1;
    pthread_mutex_unlock(&g_fd_mu);
    if (old >= 0) shutdown(old, SHUT_RDWR);   /* 唤醒旧 reader, 由它自己 close */

    ConnCtx *c = (ConnCtx *)malloc(sizeof(*c));
    strncpy(c->host, host, sizeof(c->host) - 1); c->host[sizeof(c->host) - 1] = 0;
    c->port = g_port; c->gen = gen;
    pthread_t t;
    if (pthread_create(&t, NULL, reader_thread, c) == 0) pthread_detach(t);
    else { free(c); deliver_bridge(FALSE, "无法创建网络线程"); }
}

/* ---------- JS -> C 命令处理(主线程) ---------- */
static void on_script_message(WebKitUserContentManager *ucm, WebKitJavascriptResult *res, gpointer u){
    (void)ucm; (void)u;
    JSCValue *v = webkit_javascript_result_get_js_value(res);
    char *s = jsc_value_to_string(v);
    if (!s) return;
    cJSON *root = cJSON_Parse(s);
    g_free(s);
    if (!root) return;

    const char *cmd = json_str(root, "cmd");
    if (!cmd){ cJSON_Delete(root); return; }

    if (!strcmp(cmd, "connect")){
        const char *host = json_str(root, "host");
        if (!host || !*host) host = "127.0.0.1";
        start_connect(host);
    } else if (!strcmp(cmd, "send")){
        cJSON *msg = cJSON_GetObjectItem(root, "msg");
        if (msg){
            Message m;
            memset(&m, 0, sizeof(m));
            m.type     = (uint32_t)json_num(msg, "type");
            m.status   = (uint32_t)json_num(msg, "status");
            m.group_id = (uint32_t)json_num(msg, "group_id");
            copy_field(m.from_name, json_str(msg, "from_name"));
            copy_field(m.to_name,   json_str(msg, "to_name"));
            copy_field(m.from_nick, json_str(msg, "from_nick"));
            copy_field(m.timestamp, json_str(msg, "timestamp"));
            const char *bb = json_str(msg, "bodyB64");
            if (bb){
                gsize n = 0; guchar *raw = g_base64_decode(bb, &n);
                if (n > MAX_BODY_LEN) n = MAX_BODY_LEN;
                memcpy(m.body, raw, n); m.body_len = (uint32_t)n; g_free(raw);
            } else {
                const char *b = json_str(msg, "body");
                if (b){ size_t n = strlen(b); if (n > MAX_BODY_LEN) n = MAX_BODY_LEN;
                        memcpy(m.body, b, n); m.body_len = (uint32_t)n; }
            }
            pthread_mutex_lock(&g_fd_mu); int fd = g_fd; pthread_mutex_unlock(&g_fd_mu);
            if (fd >= 0) send_msg(fd, &m);
        }
    } else if (!strcmp(cmd, "win")){
        const char *op = json_str(root, "op");
        if (op && g_win){
            if (!strcmp(op, "min")) gtk_window_iconify(GTK_WINDOW(g_win));
            else if (!strcmp(op, "max")){
                if (gtk_window_is_maximized(GTK_WINDOW(g_win))) gtk_window_unmaximize(GTK_WINDOW(g_win));
                else gtk_window_maximize(GTK_WINDOW(g_win));
            } else if (!strcmp(op, "close")) gtk_window_close(GTK_WINDOW(g_win));
        }
    } else if (!strcmp(cmd, "drag")){
        if (g_win){
            int x = (int)json_num(root, "x"), y = (int)json_num(root, "y");
            gtk_window_begin_move_drag(GTK_WINDOW(g_win), 1, x, y, gtk_get_current_event_time());
        }
    } else if (!strcmp(cmd, "report")){           /* 自检结果 */
        cJSON *ok = cJSON_GetObjectItem(root, "ok");
        const char *info = json_str(root, "info");
        int good = ok && cJSON_IsTrue(ok);
        printf("SELFTEST %s %s\n", good ? "PASS" : "FAIL", info ? info : "");
        fflush(stdout);
        cJSON_Delete(root);
        exit(good ? 0 : 1);
    }
    cJSON_Delete(root);
}

/* ---------- 自定义 URI scheme: app://zoo/<file> -> prototype/<file> ---------- */
static const char *mime_for(const char *path){
    const char *d = strrchr(path, '.');
    if (!d) return "application/octet-stream";
    if (!strcmp(d, ".html")) return "text/html";
    if (!strcmp(d, ".js"))   return "text/javascript";
    if (!strcmp(d, ".mjs"))  return "text/javascript";
    if (!strcmp(d, ".css"))  return "text/css";
    if (!strcmp(d, ".png"))  return "image/png";
    if (!strcmp(d, ".jpg") || !strcmp(d, ".jpeg")) return "image/jpeg";
    if (!strcmp(d, ".gif"))  return "image/gif";
    if (!strcmp(d, ".svg"))  return "image/svg+xml";
    if (!strcmp(d, ".webp")) return "image/webp";
    if (!strcmp(d, ".ico"))  return "image/x-icon";
    if (!strcmp(d, ".woff2"))return "font/woff2";
    if (!strcmp(d, ".woff")) return "font/woff";
    if (!strcmp(d, ".ttf"))  return "font/ttf";
    if (!strcmp(d, ".json")) return "application/json";
    return "application/octet-stream";
}
static void uri_scheme_cb(WebKitURISchemeRequest *req, gpointer user){
    (void)user;
    const char *path = webkit_uri_scheme_request_get_path(req);   /* 形如 "/zoo-chat.html" */
    if (!path || !*path || !strcmp(path, "/")) path = "/zoo-chat.html";
    while (*path == '/') path++;
    if (strstr(path, "..")){                                       /* 防目录穿越 */
        GError *e = g_error_new_literal(g_quark_from_static_string("zoo-app"), 403, "forbidden");
        webkit_uri_scheme_request_finish_error(req, e); g_error_free(e); return;
    }
    char full[8192];
    snprintf(full, sizeof(full), "%s/%s", g_proto_dir, path);
    gchar *contents = NULL; gsize len = 0; GError *err = NULL;
    if (!g_file_get_contents(full, &contents, &len, &err)){
        GError *e = g_error_new(g_quark_from_static_string("zoo-app"), 404, "not found: %s", full);
        webkit_uri_scheme_request_finish_error(req, e);
        g_error_free(e); if (err) g_error_free(err); return;
    }
    GInputStream *st = g_memory_input_stream_new_from_data(contents, len, g_free);
    webkit_uri_scheme_request_finish(req, st, len, mime_for(path));
    g_object_unref(st);
}

/* ---------- 渲染截图(自检/答辩用): 加载完成后存 PNG 退出 ---------- */
static void snapshot_cb(GObject *src, GAsyncResult *res, gpointer u){
    (void)u;
    cairo_surface_t *s = webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(src), res, NULL);
    if (s){ cairo_surface_write_to_png(s, g_shot); cairo_surface_destroy(s);
            fprintf(stderr, "[zoo-client] snapshot -> %s\n", g_shot); }
    exit(s ? 0 : 1);
}
static gboolean do_snapshot(gpointer data){
    (void)data;
    webkit_web_view_get_snapshot(g_web, WEBKIT_SNAPSHOT_REGION_VISIBLE,
        WEBKIT_SNAPSHOT_OPTIONS_NONE, NULL, snapshot_cb, NULL);
    return G_SOURCE_REMOVE;
}
static void on_load_changed(WebKitWebView *w, WebKitLoadEvent ev, gpointer u){
    (void)w; (void)u;
    if (ev == WEBKIT_LOAD_FINISHED && g_shot[0]) g_timeout_add(g_shot_delay, do_snapshot, NULL);
}

/* ---------- 定位 prototype 目录 ---------- */
static int has_html(const char *dir){
    char p[8192]; snprintf(p, sizeof(p), "%s/zoo-chat.html", dir);
    return access(p, F_OK) == 0;
}
static void set_proto(const char *cand){
    if (!realpath(cand, g_proto_dir)){
        strncpy(g_proto_dir, cand, sizeof(g_proto_dir) - 1);
        g_proto_dir[sizeof(g_proto_dir) - 1] = 0;
    }
}
static void resolve_proto_dir(void){
    const char *env = getenv("ZOO_PROTO_DIR");
    if (env && has_html(env)){ set_proto(env); return; }

    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0){
        exe[n] = 0;
        char tmp[4096]; strncpy(tmp, exe, sizeof(tmp)-1); tmp[sizeof(tmp)-1]=0;
        char *dir = dirname(tmp);
        char cand[5200];
        snprintf(cand, sizeof(cand), "%s/../prototype", dir);
        if (has_html(cand)){ set_proto(cand); return; }
        snprintf(cand, sizeof(cand), "%s/prototype", dir);
        if (has_html(cand)){ set_proto(cand); return; }
    }
    if (has_html("prototype"))    { set_proto("prototype"); return; }
    if (has_html("../prototype")) { set_proto("../prototype"); return; }
    fprintf(stderr, "[zoo-client] 警告: 未找到 prototype/zoo-chat.html, 请设置 ZOO_PROTO_DIR\n");
}

int main(int argc, char **argv){
    gtk_init(&argc, &argv);
    signal(SIGPIPE, SIG_IGN);

    const char *pe = getenv("CHAT_PORT"); if (pe && atoi(pe) > 0) g_port = atoi(pe);
    const char *shot = getenv("ZOO_SHOT"); if (shot && *shot){ strncpy(g_shot, shot, sizeof(g_shot)-1); }
    const char *sd = getenv("ZOO_SHOT_DELAY"); if (sd && atoi(sd) > 0) g_shot_delay = (guint)atoi(sd);
    resolve_proto_dir();
    fprintf(stderr, "[zoo-client] proto dir = %s, port = %d\n", g_proto_dir, g_port);

    /* 注册 app:// 自定义协议(提供 prototype 站点) */
    WebKitWebContext *ctx = webkit_web_context_get_default();
    webkit_web_context_register_uri_scheme(ctx, "app", uri_scheme_cb, NULL, NULL);
    WebKitSecurityManager *sm = webkit_web_context_get_security_manager(ctx);
    webkit_security_manager_register_uri_scheme_as_cors_enabled(sm, "app");

    /* 用户内容管理器: 注入 shim + 注册消息处理器 */
    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    webkit_user_content_manager_register_script_message_handler(ucm, "zoo");
    g_signal_connect(ucm, "script-message-received::zoo", G_CALLBACK(on_script_message), NULL);
    WebKitUserScript *shim = webkit_user_script_new(
        SHIM_JS, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    webkit_user_content_manager_add_script(ucm, shim);
    if (getenv("ZOO_SELFTEST")){
        WebKitUserScript *st = webkit_user_script_new(
            SELFTEST_JS, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, NULL, NULL);
        webkit_user_content_manager_add_script(ucm, st);
    }
    if (getenv("ZOO_AUTOLOGIN")){
        WebKitUserScript *al = webkit_user_script_new(
            AUTOLOGIN_JS, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, NULL, NULL);
        webkit_user_content_manager_add_script(ucm, al);
    }

    /* 无边框窗口(只保留页面内的一层标题栏) */
    g_win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_win), "zoo");
    gtk_window_set_default_size(GTK_WINDOW(g_win), 1200, 800);
    gtk_widget_set_size_request(g_win, 940, 600);
    gtk_window_set_decorated(GTK_WINDOW(g_win), FALSE);
    g_signal_connect(g_win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    g_web = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
                "web-context", ctx, "user-content-manager", ucm, NULL));
    WebKitSettings *stg = webkit_web_view_get_settings(g_web);
    webkit_settings_set_javascript_can_access_clipboard(stg, TRUE);
    webkit_settings_set_enable_developer_extras(stg, TRUE);

    g_signal_connect(g_web, "load-changed", G_CALLBACK(on_load_changed), NULL);
    gtk_container_add(GTK_CONTAINER(g_win), GTK_WIDGET(g_web));
    webkit_web_view_load_uri(g_web, "app://zoo/zoo-chat.html");
    gtk_widget_show_all(g_win);
    gtk_widget_grab_focus(GTK_WIDGET(g_web));

    gtk_main();
    return 0;
}
