#!/usr/bin/env bash
# ============================================================
#  zoo - 服务器一键启动 (WSL / Linux)
#
#  在 WSL(Ubuntu) 或服务器机器上:
#     bash scripts/wsl_up.sh
#  之后在桌面客户端(client/, C+WebKitGTK, bin/zoo-client)登录页「服务器地址」填本机 IP 即可。
#
#  这个脚本会:
#    1) 启动 MariaDB                (service)
#    2) 建库 chat_linux + 账号 chat (幂等, 可重复跑; 含 email 列迁移)
#    3) 编译 bin/chat_server
#    4) 前台运行 chat_server(:8888)  (Ctrl-C 退出)
#
#  可用环境变量覆盖: CHAT_DB_USER / CHAT_DB_PASS / CHAT_DB_NAME
# ============================================================
set -e
cd "$(dirname "$0")/.."

DB_USER=${CHAT_DB_USER:-chat}
DB_PASS=${CHAT_DB_PASS:-chatpw}
DB_NAME=${CHAT_DB_NAME:-chat_linux}

echo "[1/4] 启动 MariaDB ..."
sudo service mariadb start 2>/dev/null || sudo service mysql start 2>/dev/null || \
  echo "    (若未安装: sudo apt install -y mariadb-server)"

echo "[2/4] 建库 + 账号 ($DB_NAME / $DB_USER) ..."
sudo mariadb < sql/init.sql
# 老库迁移(幂等): 补 email 列与唯一索引
sudo mariadb "$DB_NAME" <<'MIG' 2>/dev/null || true
ALTER TABLE users ADD COLUMN email VARCHAR(64) NULL;
CREATE UNIQUE INDEX uniq_email ON users(email);
MIG
sudo mariadb <<SQL
CREATE USER IF NOT EXISTS '${DB_USER}'@'127.0.0.1' IDENTIFIED BY '${DB_PASS}';
GRANT ALL PRIVILEGES ON ${DB_NAME}.* TO '${DB_USER}'@'127.0.0.1';
FLUSH PRIVILEGES;
SQL

echo "[3/4] 编译 chat_server ..."
make server

echo "[4/4] 运行 chat_server(:8888)  (Ctrl-C 退出) ..."
echo "    桌面客户端: 另开终端 make client && bin/zoo-client"
echo "    登录页「服务器地址」填本机 IP(本机用 127.0.0.1; 局域网用 ip addr 查到的地址)"
echo ""
exec env CHAT_DB_HOST=127.0.0.1 CHAT_DB_USER="$DB_USER" CHAT_DB_PASS="$DB_PASS" CHAT_DB_NAME="$DB_NAME" \
  bin/chat_server 8888
