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
| 群成员 | 33 | |
| 历史 | 40 / 41 | 私聊 / 群聊 |
| 好友申请 | 25 | |
| 上下线通知 | 60 / 61 | |
| 建群 | 30 | |

## 关于「真实猛兽派对 3D 模型」

当前头像用的是开源的 3D 渲染动物头像（Microsoft Fluent 3D，已下载到 `assets/`，离线可用）。
《猛兽派对 / Party Animals》游戏官方的角色模型与动画是**受版权保护的商业资产，无法合法下载或打包进本仓库**。

如需更接近游戏的全身 3D 角色 + 动画，有两条可选路径（需你提供素材或确认授权）：
1. 你提供**授权的角色素材**（图片序列 / Lottie 动画 / glTF(`.glb`) 模型），放进 `assets/`，我接一个 Three.js / Lottie 查看器替换现有 CSS 角色；
2. 使用 **CC0/开源**的 3D 动物模型（非官方角色）做近似。

桥接服务已支持 `.glb` 静态资源（`model/gltf-binary`），可直接放模型。
