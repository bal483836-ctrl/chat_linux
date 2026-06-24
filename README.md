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

## 并发模型 (线程池 / 进程池)

服务器支持三种可切换的并发模型，通过环境变量 `CHAT_MODE` 选择：

| `CHAT_MODE`  | 模型                | 说明                                               |
|--------------|---------------------|----------------------------------------------------|
| `thread`     | 每连接一线程 (默认) | 每次 `accept` 都 `pthread_create` 一个线程，原始实现 |
| `threadpool` | 单进程 + 线程池     | 固定线程数 + 有界任务队列，复用线程、限制并发上限    |
| `process`    | pre-fork 进程池     | `fork` 出多个子进程，每个子进程各自带一个线程池      |

相关环境变量：

```bash
CHAT_MODE=threadpool CHAT_THREADS=8 bin/chat_server        # 8 线程的线程池
CHAT_MODE=process    CHAT_WORKERS=4 CHAT_THREADS=4 bin/chat_server  # 4 进程 × 4 线程
```

- `CHAT_THREADS` 线程池线程数（默认 = CPU 核数 × 2）
- `CHAT_WORKERS` 进程池子进程数，仅 `process` 模式有效（默认 = CPU 核数）

> ⚠ `process` 模式下各子进程地址空间独立、在线表与 socket 不共享，连接到
> *不同* 子进程的两个用户无法互相实时推送消息（离线消息经 MySQL 仍可达，
> 重新登录即可收到）。它主要用于演示 pre-fork 并发模型本身；要完整的实时
> 多用户聊天请用 `thread` 或 `threadpool` 单进程模式。实现细节见
> `server/threadpool.c` 与 `server/procpool.c`。

## 协议速览

所有消息共用一个固定大小 `Message` 结构 (`common/protocol.h`)，用 `type` 字段区分语义。详细类型表见 `protocol.h` 中的 `enum MsgType`。

## 关键并发点

- 在线表 (`server/online.c`) — `pthread_mutex_t`
- MySQL 单连接 (`server/db.c`) — `pthread_mutex_t`
- 线程池任务队列 (`server/threadpool.c`) — `pthread_mutex_t` + 条件变量 (生产者-消费者)
- 进程池 pre-fork (`server/procpool.c`) — 多子进程共享监听 fd, 内核负载均衡 `accept`
- 客户端 socket 写 (`client/net.c`) — `pthread_mutex_t`
- GTK 跨线程刷新 — `g_idle_add` 投递到主线程

## License

教学示例，按需取用。
