# zoo · 桌面客户端界面与资源

猛兽派对风格的聊天界面，供 `desktop/`（Electron）桌面客户端加载。

| 文件 | 说明 |
|------|------|
| `zoo-chat.html` | 界面 + 全部交互逻辑 |
| `zoo-net.js`    | 网络层：通过 Electron 主进程(`window.zooNative`)与 C 服务器收发 |
| `assets/*.png`  | 本地 3D 角色头像（占位） |
| `assets/pa/`    | 放真实《猛兽派对》角色图覆盖占位（命名见目录内 README.txt） |
| `test_full.js` / `test_backend.js` | 对真实 chat_server + MySQL 的集成测试 |

## 运行
界面不单独运行，由桌面客户端加载：见 `desktop/README.md`（`cd desktop && npm install && npm start`）。

> 直接用浏览器打开 `zoo-chat.html`（`file://`）只会进入**本地演示模式**（假数据，便于看 UI），
> 不连服务器；真实收发请用桌面客户端。

## 头像 / 真实角色
加载顺序：`assets/pa/<key>.png` → `assets/<key>.png`(占位) → emoji。
把授权的角色 PNG 放进 `assets/pa/`（`cat/dog/wolf/...` 命名）即可全局替换。

## 测试（服务器运行时）
```bash
node prototype/test_full.js      # 多客户端全功能：注册/登录(账号或邮箱)/好友/私聊/群/历史/离线/搜索/通知/拉黑/删好友
node prototype/test_backend.js   # 子集：备注/邀请/群成员/群公告/资料
```
