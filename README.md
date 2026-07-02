# zoo (chat_linux)

《Linux 程序设计课程设计》— 基于多线程并发的网络即时通信工具。

- **服务器**: C + pthread + Socket + MySQL，多线程并发，互斥锁保护共享资源
- **桌面客户端**: C + WebKitGTK（窗口宿主 + TCP + 协议编解码全用 C），内嵌猛兽派对风格界面
- **功能**: 注册/登录(账号或邮箱)、私聊、群聊、好友(申请/备注/拉黑/删除)、群(建群/邀请/公告)、
  离线消息、历史记录、上下线通知、消息检索、个人资料、头像更换

```
WebKitGTK WebView(界面 HTML/CSS/JS)
      │ window.zooNative (postMessage / run_javascript 桥)
C 宿主进程(client/main.c) ── TCP(Message 4240B) ──▶ C 服务器(:8888) ──▶ MySQL
```

## 目录结构
```
common/    协议定义与可靠收发 (protocol.h / net_io.c)
server/    多线程 C 服务器
client/    C + WebKitGTK 桌面宿主 (main.c)
prototype/ 桌面客户端界面与资源 (zoo-chat.html / zoo-net.js / assets)
sql/       MySQL schema
scripts/   一键启动脚本
docs/      设计文档 / 测试用例
```

## 一、启动服务器 (Linux / WSL)
```bash
sudo apt install -y build-essential pkg-config default-libmysqlclient-dev libssl-dev mariadb-server
bash scripts/wsl_up.sh          # 起库 + 建账号 + 编译 + 运行 chat_server(:8888)
```
> 手动方式: `mysql -u root -p < sql/init.sql` 后 `make server`，再
> `CHAT_DB_USER=root CHAT_DB_PASS=你的密码 bin/chat_server`。

## 二、运行桌面客户端 (C + WebKitGTK)
```bash
sudo apt install -y libgtk-3-dev libwebkit2gtk-4.1-dev libcjson-dev
make client        # 编译 -> bin/zoo-client
bin/zoo-client     # 打开 zoo 桌面窗口
```
登录页「服务器地址」填服务器机器 IP（本机 `127.0.0.1`；局域网填服务器 `ip addr` 的地址）。
依赖/打包/排错见 `client/README.md`（OpenEuler 等旧发行版用 `webkit2gtk-4.0`，Makefile 会自动探测）。

## 协议
所有交互共用固定大小 `Message` 结构 (`common/protocol.h`)，`type` 字段区分语义。

## 测试
服务器运行时：
```bash
node prototype/test_full.js      # 后端全功能多客户端集成测试
```

## 关键并发点
- 在线表 (`server/online.c`) — `pthread_mutex_t`
- MySQL 单连接 (`server/db.c`) — `pthread_mutex_t`
- 每客户端一个工作线程 (`server/handler.c`)

## License
教学示例，按需取用。头像占位图为开源 3D emoji；真实角色图请放入 `prototype/assets/pa/`(自有/授权)。
