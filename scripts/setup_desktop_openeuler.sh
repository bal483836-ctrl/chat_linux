#!/usr/bin/env bash
# ============================================================
#  chat_linux - zoo 桌面版(Electron) openEuler 一键安装
#
#  背景: openEuler 默认 Node 太老(v12), Electron 31 需要 Node>=18,
#        所以这里自动从国内镜像下载一个独立的 Node 20, 不动系统 Node,
#        再用它安装 electron 并配好 PATH。
#
#  用法:
#    bash scripts/setup_desktop_openeuler.sh
#  跑完按提示开新终端启动即可。
# ============================================================
set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"

NODE_VER=v20.18.0
ARCH=$(uname -m)
case "$ARCH" in
  x86_64)  NARCH=x64 ;;
  aarch64) NARCH=arm64 ;;
  *) echo "不支持的架构: $ARCH"; exit 1 ;;
esac
NODE_DIR="node-${NODE_VER}-linux-${NARCH}"
NODE_HOME="$HOME/.local/$NODE_DIR"

echo "[1/4] 准备 Node ${NODE_VER} (独立安装, 不影响系统 Node) ..."
if [ ! -x "$NODE_HOME/bin/node" ]; then
  mkdir -p "$HOME/.local"
  command -v curl >/dev/null 2>&1 || sudo dnf install -y curl
  URL="https://cdn.npmmirror.com/binaries/node/${NODE_VER}/${NODE_DIR}.tar.xz"
  echo "    下载: $URL"
  curl -fL -o /tmp/node20.tar.xz "$URL"
  tar -xf /tmp/node20.tar.xz -C "$HOME/.local"
else
  echo "    已存在, 跳过下载"
fi
export PATH="$NODE_HOME/bin:$PATH"
echo "    node $(node -v) / npm $(npm -v)"

echo "[2/4] 安装 Electron 运行库 (dnf) ..."
sudo dnf install -y nss atk at-spi2-atk at-spi2-core cups-libs libdrm \
    mesa-libgbm alsa-lib libXScrnSaver libxshmfence libXtst || true

echo "[3/4] 安装 electron (desktop/) ..."
cd "$ROOT/desktop"
rm -rf node_modules package-lock.json
npm install --omit=dev

echo "[4/4] 把 Node 20 写入 ~/.bashrc (让以后的终端也能用) ..."
LINE="export PATH=\"$NODE_HOME/bin:\$PATH\""
grep -qF "$NODE_HOME/bin" "$HOME/.bashrc" 2>/dev/null || echo "$LINE" >> "$HOME/.bashrc"

echo ""
echo "================ 安装完成! ================"
echo "启动桌面版(两步):"
echo "  1) 确保 C 服务器在跑:"
echo "       (没跑就在仓库根目录执行)"
echo "       CHAT_DB_USER=chat CHAT_DB_PASS=chatpw ./bin/chat_server &"
echo "  2) 开一个【新终端】(让新 Node 生效), 然后:"
echo "       cd $ROOT/desktop"
echo "       npm start -- --no-sandbox"
echo "==========================================="
