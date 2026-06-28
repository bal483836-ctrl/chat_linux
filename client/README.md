# zoo · 桌面客户端 (C + WebKitGTK)

桌面宿主用 **C** 编写：负责开窗口、TCP 连接 C 服务器、4240 字节 `Message`
协议编解码；界面仍是 `prototype/zoo-chat.html`（HTML/CSS/JS），由内嵌的
**WebKitGTK WebView** 渲染。UI 与其交互逻辑完全复用，一行未改。

```
WebKitGTK WebView (prototype/zoo-chat.html + zoo-net.js)
      │  window.zooNative  (宿主注入的桥)
      │   - JS -> C : window.webkit.messageHandlers.zoo.postMessage(JSON)
      │   - C -> JS : run_javascript( window.__zooDeliver("base64-json") )
client/main.c (C 宿主)  ──  TCP (Message 4240B)  ──▶  C 服务器 :8888  ──▶  MySQL
```

## 工作原理
- **站点提供**：宿主注册自定义协议 `app://zoo/...`，把 `prototype/` 目录当作站点
  提供给 WebView（有稳定 origin，`localStorage`「记住密码/服务器地址」可用）。
- **桥接**：在页面 `document-start` 注入一段 shim，重建 `window.zooNative`
  （`connect/send/onMsg/win`），所以 `zoo-net.js`、`zoo-chat.html` 无需改动。
- **网络**：每次 `connect` 起一个接收线程做 TCP 连接与 `recv_msg` 循环；
  收到帧后经 `g_idle_add` 交回 GTK 主线程投递给 JS（线程安全）。
- **协议**：`encode/decode` 复用 `common/protocol.h` + `common/net_io.c`，
  body 走 base64 传输，中文/emoji/二进制都不丢。
- **窗口**：无边框（只保留页面内一层标题栏），标题栏的最小化/最大化/关闭与
  拖动都由 C 通过 GTK 实现。

## 依赖 (Ubuntu/Debian)
```bash
sudo apt install -y libgtk-3-dev libwebkit2gtk-4.1-dev libcjson-dev
```
> OpenEuler / 旧发行版若没有 `webkit2gtk-4.1`，安装 `webkit2gtk-4.0`（与
> `gtk3-devel`、`cjson-devel`）即可，顶层 `Makefile` 会自动探测版本。

## 编译 & 运行
```bash
make client        # 产出 bin/zoo-client
bin/zoo-client     # 打开 zoo 桌面窗口
```
登录页「服务器地址」填服务器机器 IP（本机 `127.0.0.1`；局域网填服务器的
`ip addr` 地址）。

## 环境变量
| 变量 | 作用 |
|------|------|
| `CHAT_PORT`      | 服务器端口（默认 8888） |
| `ZOO_PROTO_DIR`  | `prototype` 目录位置（默认按可执行文件位置自动推断） |
| `ZOO_SELFTEST=1` | 启动后自动跑一遍 注册/登录/私聊/历史/建群/资料 自检，打印 `SELFTEST PASS/FAIL` 后退出 |
| `ZOO_SHOT=path`  | 渲染完成后截图存到 `path` 并退出（答辩/测试用） |

### 无显示器环境自检（CI / 服务器）
```bash
ZOO_SELFTEST=1 xvfb-run -a bin/zoo-client      # 需先起好 chat_server + MySQL
```

## 打包
- 直接分发 `bin/zoo-client` + `prototype/` 目录（保持相对位置 `../prototype`，
  或用 `ZOO_PROTO_DIR` 指定）。
- 需要单文件可用 AppImage：把 `bin/zoo-client` 与 `prototype/` 一起打入
  AppDir，按上面的目录约定放置即可。
