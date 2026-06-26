#!/usr/bin/env bash
# ============================================================
#  chat_linux - openEuler / RHEL 系 一键准备脚本
#
#  做四件事(都可重复跑):
#    1) 用 dnf 装依赖 (gcc/make/gtk3/openssl/MariaDB)
#    2) 启动 MariaDB 服务 (自动识别 mariadb / mysqld)
#    3) 建库 chat_linux + 专用账号
#    4) 修正头文件路径(若需要) 并编译 (用 -lmariadb 链接)
#
#  用法:
#    bash scripts/setup_openeuler.sh
#
#  可用环境变量覆盖:
#    CHAT_DB_USER (默认 chat) / CHAT_DB_PASS (默认 chatpw) / CHAT_DB_NAME (默认 chat_linux)
# ============================================================
set -e
cd "$(dirname "$0")/.."

DB_USER=${CHAT_DB_USER:-chat}
DB_PASS=${CHAT_DB_PASS:-chatpw}
DB_NAME=${CHAT_DB_NAME:-chat_linux}

echo "[1/5] 安装依赖 (dnf) ..."
sudo dnf groupinstall -y "Development Tools"
sudo dnf install -y gtk3-devel openssl-devel mariadb-server mariadb mariadb-connector-c-devel

echo "[2/5] 启动数据库服务 ..."
SVC=mariadb
if ! systemctl list-unit-files 2>/dev/null | grep -q '^mariadb\.service'; then
  if systemctl list-unit-files 2>/dev/null | grep -q '^mysqld\.service'; then
    SVC=mysqld
  fi
fi
echo "    服务名: $SVC"
sudo systemctl enable --now "$SVC"

echo "[3/5] 建库 + 账号 ($DB_NAME / $DB_USER) ..."
sudo mysql < sql/init.sql
# 关键: MariaDB 会把 127.0.0.1 反解析成 localhost, 所以三种 host 都建一遍,
# 避免出现 "Access denied for user 'chat'@'localhost'" 这种坑。
sudo mysql <<SQL
CREATE USER IF NOT EXISTS '${DB_USER}'@'localhost' IDENTIFIED BY '${DB_PASS}';
CREATE USER IF NOT EXISTS '${DB_USER}'@'127.0.0.1' IDENTIFIED BY '${DB_PASS}';
CREATE USER IF NOT EXISTS '${DB_USER}'@'%'         IDENTIFIED BY '${DB_PASS}';
ALTER USER '${DB_USER}'@'localhost' IDENTIFIED BY '${DB_PASS}';
ALTER USER '${DB_USER}'@'127.0.0.1' IDENTIFIED BY '${DB_PASS}';
GRANT ALL PRIVILEGES ON ${DB_NAME}.* TO '${DB_USER}'@'localhost';
GRANT ALL PRIVILEGES ON ${DB_NAME}.* TO '${DB_USER}'@'127.0.0.1';
GRANT ALL PRIVILEGES ON ${DB_NAME}.* TO '${DB_USER}'@'%';
FLUSH PRIVILEGES;
SQL

echo "[4/5] 检查头文件路径 ..."
if [ ! -e /usr/include/mysql/mysql.h ] && [ -e /usr/include/mariadb/mysql.h ]; then
  echo "    /usr/include/mysql 不存在, 链接到 mariadb 头目录"
  sudo ln -sf /usr/include/mariadb /usr/include/mysql
fi

echo "[5/5] 编译 (链接 -lmariadb) ..."
make clean >/dev/null 2>&1 || true
make SRV_LD="-lpthread -lmariadb -lssl -lcrypto"

echo ""
echo "================ 全部就绪! 你现在只需要做两件事 ================"
echo "  1) 启动服务端(后台):"
echo "       bash scripts/run_server.sh &"
echo "  2) 启动客户端(二选一):"
echo "       ./bin/chat_client                 # 原生 GTK 客户端"
echo "       cd desktop && npm start -- --no-sandbox   # zoo 桌面版(需先装好 Node20)"
echo ""
echo "  想本机自测: 再开一个终端多起一个 ./bin/chat_client, 两个号互相加好友聊天。"
echo "==============================================================="
