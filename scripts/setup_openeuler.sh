#!/usr/bin/env bash
# ============================================================
#  zoo (chat_linux) - openEuler / RHEL 系 一键部署
#  分支: claude/awesome-thompson-czjudu (C 服务器 + C/WebKitGTK 桌面客户端)
#
#  做完这一步, 你只需要再运行 服务端 + 客户端:
#    bash scripts/run_server.sh &
#    ./bin/zoo-client
#
#  本脚本(可重复跑):
#    1) dnf 装依赖 (服务器 + WebKitGTK 客户端)
#    2) 启动 MariaDB
#    3) 建库 + 数据库账号 (localhost/127.0.0.1 都建, 避开 access denied 坑)
#    4) 修正 mysql 头文件路径(若需要)
#    5) 编译 bin/chat_server 和 bin/zoo-client
#
#  可用环境变量: CHAT_DB_USER(chat) CHAT_DB_PASS(chatpw) CHAT_DB_NAME(chat_linux)
# ============================================================
set -e
cd "$(dirname "$0")/.."

DB_USER=${CHAT_DB_USER:-chat}
DB_PASS=${CHAT_DB_PASS:-chatpw}
DB_NAME=${CHAT_DB_NAME:-chat_linux}

echo "[1/5] 安装依赖 (dnf) ..."
sudo dnf groupinstall -y "Development Tools"
# 服务器依赖
sudo dnf install -y openssl-devel mariadb-server mariadb
# MySQL/MariaDB 开发库(mysql.h + 客户端库): 系统若已有(mysql-devel 或
# mariadb-connector-c-devel)就跳过, 避免二者都提供 mysql_config/头文件导致的
# "Transaction test error: file ... conflicts" 冲突。
if command -v mysql_config >/dev/null 2>&1 || [ -e /usr/include/mysql/mysql.h ] || [ -e /usr/include/mariadb/mysql.h ]; then
  echo "    已检测到 MySQL/MariaDB 开发库, 跳过安装(避免文件冲突)"
else
  sudo dnf install -y mariadb-connector-c-devel || sudo dnf install -y mysql-devel
fi
# 客户端依赖 (GTK3 + WebKitGTK + cJSON); webkit 包名各版本不同, 逐个尝试
sudo dnf install -y gtk3-devel cjson-devel
sudo dnf install -y webkit2gtk3-devel \
  || sudo dnf install -y webkit2gtk4.1-devel \
  || sudo dnf install -y webkitgtk4.1-devel \
  || echo "  !! 未自动装上 WebKitGTK, 请手动: sudo dnf search webkit  然后装 *-devel 包"

echo "[2/5] 启动数据库服务 ..."
SVC=mariadb
if ! systemctl list-unit-files 2>/dev/null | grep -q '^mariadb\.service'; then
  systemctl list-unit-files 2>/dev/null | grep -q '^mysqld\.service' && SVC=mysqld
fi
echo "    服务名: $SVC"
sudo systemctl enable --now "$SVC"

echo "[3/5] 建库 + 账号 ($DB_NAME / $DB_USER) ..."
sudo mysql < sql/init.sql
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

echo "[4/5] 检查 mysql 头文件路径 ..."
if [ ! -e /usr/include/mysql/mysql.h ] && [ -e /usr/include/mariadb/mysql.h ]; then
  echo "    链接 /usr/include/mysql -> mariadb 头目录"
  sudo ln -sf /usr/include/mariadb /usr/include/mysql
fi

echo "[5/5] 编译 服务器 + 桌面客户端 ..."
make clean >/dev/null 2>&1 || true
# 服务器: Makefile 会用 mysql_config/mariadb_config 自动探测 MySQL 库路径
# (含 -L, 兼容 openEuler 把库放在 /usr/lib64/mysql 的情况), 无需手写 SRV_LD
make server
# 客户端: C + WebKitGTK, Makefile 自动探测 webkit2gtk-4.1/4.0
make client

echo ""
echo "================ 全部就绪! 你现在只需要做两件事 ================"
echo "  1) 启动服务端(后台):"
echo "       bash scripts/run_server.sh &"
echo "  2) 启动 zoo 桌面客户端:"
echo "       ./bin/zoo-client"
echo "     (登录页\"服务器地址\"填 127.0.0.1)"
echo ""
echo "  自测(可选, 无界面也能跑): ZOO_SELFTEST=1 xvfb-run -a ./bin/zoo-client"
echo "==============================================================="
