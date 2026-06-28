#!/usr/bin/env bash
# ============================================================
#  一键启动 chat_server, 自动带上正确的数据库账号/密码,
#  省得每次手敲一长串 CHAT_DB_* 环境变量(还容易打错)。
#
#  用法:
#    bash scripts/run_server.sh        # 前台运行(看日志, Ctrl-C 停)
#    bash scripts/run_server.sh &      # 后台运行
#
#  可用环境变量覆盖默认值:
#    CHAT_DB_HOST(127.0.0.1) CHAT_DB_USER(chat) CHAT_DB_PASS(chatpw) CHAT_DB_NAME(chat_linux)
# ============================================================
cd "$(dirname "$0")/.."

export CHAT_DB_HOST=${CHAT_DB_HOST:-127.0.0.1}
export CHAT_DB_USER=${CHAT_DB_USER:-chat}
export CHAT_DB_PASS=${CHAT_DB_PASS:-chatpw}
export CHAT_DB_NAME=${CHAT_DB_NAME:-chat_linux}

if [ ! -x bin/chat_server ]; then
  echo "未找到 bin/chat_server, 请先跑: bash scripts/setup_openeuler.sh"
  exit 1
fi

echo "启动 chat_server (库=$CHAT_DB_NAME 用户=$CHAT_DB_USER 端口=${CHAT_PORT:-8888}) ..."
exec ./bin/chat_server "$@"
