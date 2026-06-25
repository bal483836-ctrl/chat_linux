#!/usr/bin/env bash
# ============================================================
#  zoo - WSL 一键部署 / 运行
#
#  在 WSL(Ubuntu) 里:
#     bash scripts/wsl_up.sh
#  然后用 Windows 浏览器打开   http://localhost:8080/
#
#  这个脚本会:
#    1) 启动 MariaDB                (service)
#    2) 建库 chat_linux + 账号 chat (幂等, 可重复跑)
#    3) 编译 bin/chat_server        (若不存在)
#    4) 后台启动 chat_server(:8888)
#    5) 前台启动 Web 桥接(:8080)    (Ctrl-C 退出)
#
#  可用环境变量覆盖: PORT / CHAT_DB_USER / CHAT_DB_PASS / CHAT_DB_NAME
# ============================================================
set -e
cd "$(dirname "$0")/.."

DB_USER=${CHAT_DB_USER:-chat}
DB_PASS=${CHAT_DB_PASS:-chatpw}
DB_NAME=${CHAT_DB_NAME:-chat_linux}
PORT=${PORT:-8080}

echo "[1/5] 启动 MariaDB ..."
sudo service mariadb start 2>/dev/null || sudo service mysql start 2>/dev/null || \
  echo "    (若未安装: sudo apt install -y mariadb-server)"

echo "[2/5] 建库 + 账号 ($DB_NAME / $DB_USER) ..."
sudo mariadb < sql/init.sql
sudo mariadb <<SQL
CREATE USER IF NOT EXISTS '${DB_USER}'@'127.0.0.1' IDENTIFIED BY '${DB_PASS}';
GRANT ALL PRIVILEGES ON ${DB_NAME}.* TO '${DB_USER}'@'127.0.0.1';
FLUSH PRIVILEGES;
SQL

echo "[3/5] 编译 chat_server ..."
if [ ! -x bin/chat_server ]; then make server; fi

echo "[4/5] 启动 chat_server(:8888) ..."
pkill -x chat_server 2>/dev/null || true
CHAT_DB_HOST=127.0.0.1 CHAT_DB_USER="$DB_USER" CHAT_DB_PASS="$DB_PASS" CHAT_DB_NAME="$DB_NAME" \
  setsid bin/chat_server 8888 >/tmp/zoo_chat_server.log 2>&1 </dev/null &
sleep 1
if ! (exec 3<>/dev/tcp/127.0.0.1/8888) 2>/dev/null; then
  echo "    !! chat_server 未起来, 看日志: tail /tmp/zoo_chat_server.log"; tail -5 /tmp/zoo_chat_server.log || true; exit 1
fi
echo "    chat_server OK"

echo "[5/5] 启动 Web 桥接(:$PORT) ..."
echo ""
echo "    >>> 用 Windows 浏览器打开:  http://localhost:$PORT/   <<<"
echo "    (Ctrl-C 停止桥接; chat_server 仍在后台, 可用 'pkill -x chat_server' 停止)"
echo ""
PORT="$PORT" CHAT_HOST=127.0.0.1 CHAT_PORT=8888 exec node prototype/bridge.js
