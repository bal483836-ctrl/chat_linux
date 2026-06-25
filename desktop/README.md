# zoo 桌面版 (Electron)

把 zoo 界面做成**真正的桌面应用**:独立窗口、双击运行、**没有浏览器、没有 localhost**。
和 VS Code / Discord 一样,用同一套网页 UI,装进原生窗口里。

```
Electron 窗口(渲染进程 = zoo 界面)
        │  IPC
Electron 主进程(Node)──TCP(Message 4240B)──▶ C 聊天服务器(:8888)──▶ MySQL
```

主进程直接用 TCP 连 C 服务器,**不需要 bridge.js,也不占 8080 端口**。

## 文件
| 文件 | 说明 |
|------|------|
| `main.js`    | 主进程:建窗口、TCP 直连 C 服务器、JSON↔Message 互转、IPC |
| `preload.js` | 安全地给渲染进程暴露 `window.zooNative`(connect/send/onMsg) |
| `msgcodec.js`| Message 结构编解码(与 `common/protocol.h` 一致) |
| 渲染界面     | 复用 `../prototype/zoo-chat.html`(检测到 `window.zooNative` 即用桌面传输) |

## 运行(开发模式)

> 先确保 C 服务器在跑(见仓库根 `scripts/wsl_up.sh` 或 `prototype/README.md`)。
> Windows 上:C 服务器跑在 WSL 里即可,Electron 在 Windows 侧通过 `localhost` 连得到。

```bash
cd desktop
npm install          # 首次:下载 Electron
npm start            # 打开 zoo 桌面窗口
```
默认连 `127.0.0.1:8888`,可改:
```bash
# Windows PowerShell
$env:CHAT_HOST="127.0.0.1"; $env:CHAT_PORT="8888"; npm start
# Linux/WSL
CHAT_HOST=127.0.0.1 CHAT_PORT=8888 npm start
```
窗口标题栏出现「桌面版 · 已连服务器 ✅」即成功。

## 打包成安装包 / exe(可选)
```bash
cd desktop
npm install
npm run dist          # Windows: 生成 NSIS 安装包到 desktop/dist/
# 或 npm run dist:linux   # Linux: 生成 AppImage
```
打包时 `../prototype`(界面+头像)会作为 extraResources 一并打进去。

## 与浏览器版的关系
- **同一套界面代码**。`zoo-chat.html` 自动判断环境:
  - 检测到 `window.zooNative` → 桌面版,走 Electron 主进程 TCP 直连;
  - 否则在浏览器里 → 走 `prototype/bridge.js` 的 WebSocket;
  - 直接 `file://` 双击且无后端 → 演示模式(本地假数据)。
- 所以浏览器预览和桌面应用共用一份 UI,改一处两边都生效。
