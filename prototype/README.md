# zoo · 桌面客户端界面与资源

猛兽派对风格的聊天界面，供 `client/`（C + WebKitGTK）桌面宿主内嵌加载。

| 文件 | 说明 |
|------|------|
| `zoo-chat.html` | 界面 + 全部交互逻辑 |
| `zoo-net.js`    | 网络层：通过宿主注入的 `window.zooNative` 与 C 服务器收发 |
| `assets/*.png`  | 本地 3D 角色头像（占位） |
| `assets/pa/`    | 放真实《猛兽派对》角色图覆盖占位（命名见目录内 README.txt） |
| `test_full.js` / `test_backend.js` | 对真实 chat_server + MySQL 的集成测试 |

## 运行
界面不单独运行，由桌面宿主加载：见 `client/README.md`（`make client && bin/zoo-client`）。
宿主通过自定义协议 `app://zoo/zoo-chat.html` 提供本目录，并注入 `window.zooNative` 完成与 C 服务器的 TCP 收发。

> 直接用浏览器打开 `zoo-chat.html`（`file://`）只会进入**本地演示模式**（假数据，便于看 UI），
> 不连服务器；真实收发请用桌面客户端 `bin/zoo-client`。

## 头像 / 真实角色
加载顺序：`assets/pa/<key>.png` → `assets/<key>.png`(占位) → emoji。
把授权的角色 PNG 放进 `assets/pa/`（`cat/dog/wolf/...` 命名）即可全局替换。

## 测试（服务器运行时）
```bash
node prototype/test_full.js      # 多客户端全功能：注册/登录(账号或邮箱)/好友/私聊/群/历史/离线/搜索/通知/拉黑/删好友
node prototype/test_backend.js   # 子集：备注/邀请/群成员/群公告/资料
```
