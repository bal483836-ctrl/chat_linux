# zoo · Web 原型 + 后端桥接

Party-Animals 风格的桌面端聊天前端原型，**可连接仓库里的 C 聊天服务器**进行开发。

## 文件

| 文件 | 说明 |
|------|------|
| `zoo-chat.html` | 前端原型（单文件，含全部 UI 与交互） |
| `zoo-net.js`    | 浏览器网络层：WebSocket 客户端 + 协议封装/解析 |
| `bridge.js`     | **WebSocket ⇄ TCP 桥接**：静态服务 + 把浏览器 JSON 与 4240B `Message` 结构互转 |
| `assets/*.png`  | 本地 3D 角色头像（离线可用） |

## 为什么需要桥接？

C 服务器（`server/`）用的是**裸 TCP + 定长 `Message` 结构**（`common/protocol.h`，4240 字节，小端）。
浏览器只能用 WebSocket/HTTP，无法直接连裸 TCP。`bridge.js` 负责：

```
浏览器 ──WebSocket(JSON)──▶ bridge.js ──TCP(Message 4240B)──▶ chat_server(:8888) ──▶ MySQL
```

桥接是纯 Node 内置模块实现，**无需 npm install**。

## 运行（连真实后端）

```bash
# 1. 建库
mysql -u root -p < sql/init.sql

# 2. 编译并启动 C 服务器
make server
CHAT_DB_USER=root CHAT_DB_PASS=你的密码 bin/chat_server &     # 默认监听 8888

# 3. 启动 Web 桥接（默认 8080）
make web            # 等价于 scripts/run_web.sh

# 4. 浏览器打开
#    http://localhost:8080/
```

可用环境变量覆盖：`PORT`（桥接端口）、`CHAT_HOST` / `CHAT_PORT`（C 服务器地址）。

### 两种运行模式

- **实时模式**：通过 `http://localhost:8080/` 打开时，标题栏显示「已连服务器 ✅」。
  - **注册** → 服务器分配 6 位账号（`100001` 起）；**登录**用该账号 + 密码。
  - 登录后自动加载真实**好友列表 / 群列表 / 离线消息**。
  - 发送/接收**私聊、群聊**走真实服务器；打开会话拉取**历史记录**；上下线**实时通知**。
  - 加好友（输入对方账号发申请）、拉黑、删好友、建群均调用真实协议。
- **演示模式**：直接双击 `zoo-chat.html`（`file://`）打开时，使用内置假数据，所有交互本地模拟，方便离线看 UI。

## 协议对应（zoo-net.js ⇄ MsgType）

| 操作 | MsgType | 说明 |
|------|---------|------|
| 注册 / 登录 | 1 / 2 | body=`"昵称\n密码"` / `"账号\n密码"` |
| 私聊 / 群聊 | 10 / 11 | `to_name` 账号 / `group_id` 群号 |
| 好友/群列表 | 22 / 32 | 登录后服务器主动推送 |
| 群成员 | 33 | **新增**：返回 `account\tnick\tcolor\tonline` |
| 历史 | 40 / 41 | 私聊 / 群聊 |
| 好友申请 | 25 | |
| 上下线通知 | 60 / 61 | |
| 建群 | 30 | |
| 好友备注 | 29 | **新增** `MSG_FRIEND_REMARK` |
| 邀请入群 | 39 | **新增** `MSG_GROUP_INVITE`（成员把好友拉进群） |
| 群公告 | 100 | **新增** `MSG_GROUP_NOTICE`（群主设置/任意成员查询） |
| 个人资料 | 101/102/103 | **新增** `MSG_PROFILE_GET/SET/DATA`（昵称+出生日期） |

## 本次为「前端有、后端缺」的功能补的后端

前端原型里这些功能此前只在本地模拟，现已在 **C 服务器 + MySQL** 真正落地，
并在 Linux 上用真实 MariaDB 跑通自动化测试（`register→login→...` 全绿）：

| 功能 | 后端改动 |
|------|---------|
| 好友备注 | `friends.remark` 列 + `MSG_FRIEND_REMARK`；写进好友列表第 6 列 |
| 邀请好友入群 | `MSG_GROUP_INVITE`：校验邀请人是群成员后批量加人，并把新群列表推给被邀请者 |
| 群公告 | `chat_groups.notice` 列 + `MSG_GROUP_NOTICE`（仅群主可改） |
| 个人资料/出生日期 | `users.birthday` 列 + `MSG_PROFILE_GET/SET/DATA` |
| 群成员列表 | 补上 `MSG_GROUP_MEMBERS` 的服务端处理（之前协议有定义但无实现） |

> 数据库新增列已写入 `sql/init.sql`（全新安装直接生效）；
> 老库可执行：
> ```sql
> ALTER TABLE users       ADD COLUMN birthday DATE NULL;
> ALTER TABLE friends     ADD COLUMN remark   VARCHAR(32)  NOT NULL DEFAULT '';
> ALTER TABLE chat_groups ADD COLUMN notice   VARCHAR(512) NOT NULL DEFAULT '';
> ```

## 头像 / 真实猛兽派对角色

已去掉 3D 模型方案，统一用**头像图片**。头像加载顺序：

```
assets/pa/<key>.png   ← 放真实《猛兽派对》角色图(官方/授权)
        ↓ 没有则回退
assets/<key>.png      ← 仓库自带的开源 3D 黏土动物头像(占位)
        ↓ 没有则回退
emoji
```

**换成真实角色：** 把官方/授权的角色 PNG 放进 `prototype/assets/pa/`，
按 `cat/dog/wolf/croc/bear/bunny/pig/tiger/fox/panda/penguin/koala/lion/cow/frog/hamster/owl/unicorn/mouse/hippo` 命名即可，
全应用头像会立刻替换，无需改代码。详见 `assets/pa/README.txt`。

> 《猛兽派对 / Party Animals》官方角色美术是**受版权保护的商业素材**，仓库不内置；
> 请放入你自己拥有或已获授权的图片。仓库默认带的是开源黏土动物头像作占位。
