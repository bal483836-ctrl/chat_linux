# chat_linux 设计说明书

> 课程: 《Linux 程序设计课程设计》
> 题目: 基于并发的 Linux 网络即时通信工具

本文是对照课程大纲三个考核目标的设计说明。代码结构与文中术语严格一致，便于答辩对照。

---

## 1. 总体架构

```
┌────────────────────┐           ┌────────────────────────────┐
│   GTK3 客户端      │  TCP      │      多线程服务器           │
│  (chat_client)     │ ◀──────▶ │      (chat_server)         │
│ ─ 主线程 GTK 主循环 │  Socket   │ ─ 主线程 accept()           │
│ ─ recv_thread      │           │ ─ 每客户端 1 个 worker 线程 │
│   读 socket, 用    │           │ ─ 在线表 / DB 用 mutex 保护 │
│   g_idle_add 投递  │           │ ─ MySQL 持久化              │
│   回主线程         │           └────────────────────────────┘
└────────────────────┘                       │
                                              ▼
                                     ┌────────────────┐
                                     │  MySQL: 用户/  │
                                     │  好友/群/消息  │
                                     │  /离线队列     │
                                     └────────────────┘
```

源码目录:

```
common/    protocol.h  net_io.c/h         协议与可靠收发
server/    server.c    handler.c          accept 循环 / 业务派发
           online.c    db.c               在线表 / MySQL 封装
client/    main.c      net.c   ui.c       入口 / 接收线程 / GTK 界面
sql/       init.sql                       建库脚本
scripts/   setup.sh                       一键安装
```

---

## 2. 通信协议

固定大小 `Message` 结构，字段含义见 `common/protocol.h`。
- 优点: 收发简单 (一次 `read(MSG_SIZE)`)，便于讲解 Socket。
- 缺点: 每条消息固定 ~4 KB；本课程教学场景下可接受。
- 文件传输: `MSG_FILE_BEGIN` → 多个 `MSG_FILE_CHUNK` → `MSG_FILE_END`，每片 `FILE_CHUNK_SIZE = 2048` 字节。

`enum MsgType` 用一个 `type` 字段区分所有语义；服务器在 `handler.c::client_thread` 中用 `switch` 派发。

---

## 3. 并发设计：进程 vs 线程

课程要求"通过进程/线程的对比强化对并发程序设计的理解"。

| 维度 | 多进程 (fork) | 多线程 (pthread, 本项目采用) |
|------|---------------|-------------------------------|
| 创建开销 | 重 (复制 PCB+地址空间) | 轻 (共享地址空间) |
| 共享数据 | 需 IPC (共享内存/管道/消息队列) | 直接共享全局变量 |
| 隔离性 | 强 (一个崩溃不波及其他) | 弱 (一个段错误整个服务挂掉) |
| 同步原语 | SysV 信号量 / POSIX `sem_open` | `pthread_mutex_t` / `pthread_cond_t` |
| 在线表实现 | 需放共享内存 + 信号量保护 | 直接放全局数组 + `pthread_mutex_t` 保护 |

**本项目结论**: 选择 pthread。
原因：在线表 (`g_tab`) 与 MySQL 连接需在所有工作流之间共享，使用线程后无需引入共享内存与 SysV 信号量；同步只用一把 `pthread_mutex_t` (`online.c:g_mu`、`db.c:g_dbmu`)，逻辑直观。

**对照演示**: 若想体验进程版本，可改 `pthread_create` 为 `fork`，并把 `g_tab` 放进 `mmap(MAP_SHARED|MAP_ANON)` 区域、用 `sem_init(..., 1, 1)` 进程间信号量保护。本项目在 `server/server.c` 注释里也指出了这一对照点，便于答辩时讲解。

---

## 4. 同步与互斥问题

| 共享资源 | 同步原语 | 位置 |
|----------|----------|------|
| 在线用户表 `g_tab` | `pthread_mutex_t g_mu` | `server/online.c` |
| 服务器对同一 socket 的写 | `pthread_mutex_t write_mu` | `server/online.c:online_push` |
| MySQL 单连接 | `pthread_mutex_t g_dbmu` | `server/db.c` |
| 客户端对 socket 的写 | `pthread_mutex_t send_mu` | `client/net.c` |
| GTK 控件跨线程访问 | `g_idle_add` 单线程投递 | `client/net.c:dispatch_in_main` |

### 关键同步场景

1. **多个 worker 同时给同一用户推消息**
   私聊 A→B 与群聊 G→B 可能在两个线程并发执行 `send_msg(B.fd, ...)`。两次 `write` 交错会把两条 4 KB 消息拼成一坨垃圾。
   解决: `online_push` 内统一加 `write_mu` 串行化。

2. **登录覆盖在线表项**
   同一账号在第二台机器登录时，旧连接需先被替换。`online_add` 在持锁状态下 `close(old_fd) → 写入新 fd`，保证读者要么看到旧的、要么看到新的，不会读到一半。

3. **MySQL 连接非线程安全**
   `libmysqlclient` 单连接上同时执行 SQL 会发生协议串行号错乱。`g_dbmu` 把所有 SQL 调用串行化。生产环境应改用连接池，本项目教学目的足够。

4. **GTK 仅可在主线程操作**
   接收线程不能直接 `gtk_text_buffer_insert`。`g_idle_add(dispatch_in_main, pack)` 把消息体打包成 heap 对象、由 GLib 主循环弹回主线程消费，等价于课程中的"生产者-消费者"问题，队列由 GLib 自己管理。

---

## 5. 数据存储

使用 MySQL：

- `users` 用户与登录态
- `friends` 双向好友 + 单向黑名单 (`status=1` 表示 `user_id` 拉黑了 `friend_id`)
- `chat_groups` / `group_members` 群与成员
- `messages` 全量消息历史 (`msg_type 0=私聊 1=群聊`)
- `offline_msg` 离线队列：消息保存进 `messages` 后，若目标用户不在线，则向 `offline_msg` 追加一条；用户上线时 `db_offline_take` 取出并清空

所有 SQL 用 `mysql_real_escape_string` 防注入，登录密码用 `sha1` 摘要存储 (本课程教学示例；生产请用 bcrypt/argon2)。

---

## 6. 功能矩阵 (对应答辩评分标准)

| 课程大纲要求 | 实现位置 |
|--------------|----------|
| 多用户私聊 / 群聊 | `handler.c::do_private / do_group` |
| 多线程并发 | `pthread_create(..., client_thread)` |
| Socket 编程 | `server.c` / `client/net.c` |
| 同步互斥描述 | 本文第 4 节 |
| MySQL 存储 | `db.c` 全套 |
| 图形界面 (GTK) | `client/ui.c` |
| Shell 命令 | `scripts/setup.sh`，启动 `bin/chat_server &` |
| 好友与黑名单 | `MSG_FRIEND_*` / `MSG_BLACK_*` |
| 群发消息 | `do_group` → `online_push` 广播 |
| 离线消息 | `offline_msg` 表 + `db_offline_take` |
| 文件传送 | `MSG_FILE_BEGIN/CHUNK/END` 三步分块 |
| 上下线通知 | `notify_friends` 在登录/退出时广播 |

---

## 7. 运行

```bash
# 安装依赖 + 建库 + 编译
scripts/setup.sh

# 起服务器
bin/chat_server &

# 起两个客户端测试私聊/群聊
bin/chat_client &
bin/chat_client &
```

测试脚本和答辩演示用例见 `docs/demo.md` (可在课程报告中补充)。

---

## 8. 可扩展点（用于优秀档评分）

- [x] 好友/黑名单划分
- [x] 消息群发
- [x] 离线消息
- [x] 文件传送
- [x] 好友上线/下线通知
- [ ] 表情/图片消息 (按 `MSG_FILE_*` 思路扩展即可)
- [ ] 加密传输 (`MSG_LOGIN` 前协商 ECDHE 派生会话密钥, AES-GCM)
- [ ] 多进程对照版本 (sysv-shm + sysv-sem)

---

## 9. 答辩 Shell 命令清单

```bash
ps -eLf | grep chat_server          # 查看主线程 + 工作线程
ss -tnlp | grep 8888                # 监听端口
lsof -p $(pidof chat_server) | head # 已打开的 socket / DB fd
strace -p <tid> -e read,write       # 跟踪某 worker 的收发
mysql chat_linux -e 'select * from offline_msg'
journalctl --user -t chat_server    # (如果接入了 syslog)
```
