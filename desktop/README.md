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

## 连别人的电脑当服务器（局域网多人）
谁的电脑跑 `chat_server`，谁就是服务器。其它电脑上的桌面应用，在**登录页的「服务器地址」**里
填服务器电脑的 IP（如 `192.168.1.20`）即可；地址会记住。
- 服务器电脑查 IP：Windows `ipconfig`、Linux `ip addr`。
- 服务器已监听 `0.0.0.0:8888`，确保防火墙放行 8888 端口。

## 打包成可双击运行的程序
```bash
cd desktop
npm install
npm run dist          # Windows: 生成 .exe 安装包到 desktop/dist/
npm run dist:linux    # Linux: 生成 AppImage(可在 OpenEuler 22.03 直接运行)
```
打包时 `../prototype`(界面+头像)会作为 extraResources 一并打进去。

### 在 OpenEuler 22.03 LTS 运行
1. 在一台 Linux(如 WSL Ubuntu)里 `npm run dist:linux` 生成 `desktop/dist/zoo-*.AppImage`；
2. 拷到 OpenEuler，赋可执行权限后双击或命令行运行：
   ```bash
   chmod +x zoo-*.AppImage
   ./zoo-*.AppImage                       # 双击亦可
   # 若提示缺 FUSE: ./zoo-*.AppImage --appimage-extract-and-run
   ```
   首次运行可能需要：`sudo dnf install -y fuse fuse-libs`。
3. 登录页「服务器地址」填 chat_server 所在机器的 IP。

> AppImage 自带 Electron 运行时，无需在 OpenEuler 上装 Node；只要能连到 chat_server 即可使用。

## 说明
- 界面与资源在 `../prototype/`，由本应用加载（开发模式相对路径，打包时作为 extraResources）。
- 直接用浏览器打开 `prototype/zoo-chat.html` 只是本地演示(假数据)，真实收发请用本桌面应用。
