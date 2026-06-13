# chat_linux

《Linux 程序设计课程设计》参考实现 — 基于多线程并发的网络即时通信工具。

- **服务器**: C + pthread + Socket + MySQL，多线程并发，互斥锁保护共享资源
- **客户端**: C + GTK3 图形界面 + pthread 收发线程
- **功能**: 注册/登录、私聊、群聊、好友/黑名单、离线消息、文件传输、上下线通知、历史消息

详细设计与对照大纲的考核点见 [`docs/design.md`](docs/design.md)。

## 目录结构

```
common/    协议定义与可靠收发
server/    多线程服务器
client/    GTK3 客户端
sql/       MySQL schema
scripts/   一键安装脚本
docs/      设计文档
```

## 构建

依赖 (Ubuntu/Debian):

```bash
sudo apt install build-essential pkg-config \
    default-libmysqlclient-dev libssl-dev libgtk-3-dev \
    mysql-server
```

或者直接：

```bash
scripts/setup.sh         # 装依赖 + 建库 + 编译
```

手动编译：

```bash
mysql -u root -p < sql/init.sql
make                     # bin/chat_server, bin/chat_client
```

## 运行

```bash
# 默认监听 8888
CHAT_DB_USER=root CHAT_DB_PASS=yourpw bin/chat_server &

bin/chat_client          # 弹出登录窗
```

可同时启动多个客户端在本机互相聊天。

## 协议速览

所有消息共用一个固定大小 `Message` 结构 (`common/protocol.h`)，用 `type` 字段区分语义。详细类型表见 `protocol.h` 中的 `enum MsgType`。

## 关键并发点

- 在线表 (`server/online.c`) — `pthread_mutex_t`
- MySQL 单连接 (`server/db.c`) — `pthread_mutex_t`
- 客户端 socket 写 (`client/net.c`) — `pthread_mutex_t`
- GTK 跨线程刷新 — `g_idle_add` 投递到主线程

## License

教学示例，按需取用。
