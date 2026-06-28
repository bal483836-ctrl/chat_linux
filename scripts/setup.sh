#!/usr/bin/env bash
# 一键初始化: 安装依赖, 建库, 编译服务器
set -e
SUDO="${SUDO:-sudo}"

echo "[1/3] 安装依赖 (Ubuntu/Debian)"
$SUDO apt-get update
$SUDO apt-get install -y build-essential pkg-config \
    default-libmysqlclient-dev libssl-dev \
    mysql-server \
    libgtk-3-dev libwebkit2gtk-4.1-dev libcjson-dev   # 客户端(C+WebKitGTK)

echo "[2/3] 初始化数据库 (需要 MySQL root 密码)"
if [ -z "$MYSQL_PWD" ]; then
    echo "    提示: 可通过 MYSQL_PWD=xxx ./setup.sh 自动传入密码"
fi
mysql -u root -p < sql/init.sql

echo "[3/3] 编译服务器 + 客户端"
make server
make client

echo "完成. 运行方式:"
echo "    bash scripts/wsl_up.sh        # 起服务器(:8888)"
echo "    bin/zoo-client                # 打开桌面客户端 (详见 client/README.md)"
