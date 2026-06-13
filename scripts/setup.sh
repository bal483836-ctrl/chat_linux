#!/usr/bin/env bash
# 一键初始化: 安装依赖, 建库, 编译
set -e
SUDO="${SUDO:-sudo}"

echo "[1/3] 安装依赖 (Ubuntu/Debian)"
$SUDO apt-get update
$SUDO apt-get install -y build-essential pkg-config \
    default-libmysqlclient-dev libssl-dev libgtk-3-dev \
    mysql-server

echo "[2/3] 初始化数据库 (需要 MySQL root 密码)"
if [ -z "$MYSQL_PWD" ]; then
    echo "    提示: 可通过 MYSQL_PWD=xxx ./setup.sh 自动传入密码"
fi
mysql -u root -p < sql/init.sql

echo "[3/3] 编译"
make

echo "完成. 运行方式:"
echo "    bin/chat_server &"
echo "    bin/chat_client"
