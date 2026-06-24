#!/usr/bin/env bash
# ============================================================
#  启动 zoo Web 原型 + 桥接到 C 聊天服务器
#
#  前置:
#    1) MySQL 已建库:  mysql -u root -p < sql/init.sql
#    2) 编译并启动服务器:
#         make server
#         CHAT_DB_USER=root CHAT_DB_PASS=yourpw bin/chat_server &
#    3) 已安装 node (用于桥接, 无需 npm install)
#
#  然后:
#    scripts/run_web.sh
#  浏览器打开 http://localhost:8080/
# ============================================================
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"

: "${PORT:=8080}"
: "${CHAT_HOST:=127.0.0.1}"
: "${CHAT_PORT:=8888}"
export PORT CHAT_HOST CHAT_PORT

if ! command -v node >/dev/null 2>&1; then
  echo "[run_web] 需要 node, 请先安装 (apt install nodejs)"; exit 1
fi

echo "[run_web] 桥接端口 :$PORT  ->  chat_server $CHAT_HOST:$CHAT_PORT"
echo "[run_web] 打开 http://localhost:$PORT/"
exec node "$HERE/prototype/bridge.js"
