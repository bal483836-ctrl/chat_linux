# ========================================================
#  zoo (chat_linux) - 顶层 Makefile
#
#  目标:
#     make server        编译服务器      -> bin/chat_server
#     make client        编译桌面客户端  -> bin/zoo-client (C + WebKitGTK)
#     make               同 server
#     make clean         清理
#
#  桌面客户端: C + WebKitGTK, 内嵌 prototype/zoo-chat.html, 见 client/README.md。
#
#  依赖 (Ubuntu/Debian):
#     # 服务器
#     sudo apt install build-essential libmysqlclient-dev libssl-dev \
#                      pkg-config mysql-server
#     # 客户端
#     sudo apt install libgtk-3-dev libwebkit2gtk-4.1-dev libcjson-dev
#     # (OpenEuler/旧发行版若无 4.1, 用 webkit2gtk-4.0; Makefile 会自动探测)
# ========================================================

CC       ?= gcc
CFLAGS   := -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -Icommon
LDFLAGS  :=

BIN      := bin
SRC_COMMON := common/net_io.c

# --- server ---
SRV_SRC := $(SRC_COMMON) server/server.c server/handler.c server/online.c server/db.c
# MySQL/MariaDB 客户端库: 优先用 mysql_config/mariadb_config 探测带 -L 路径的链接参数
# (openEuler 的 mysql-devel 把 libmysqlclient 放在 /usr/lib64/mysql, 直接 -lmysqlclient
#  会报 "找不到 -lmysqlclient"); 探测不到时退回 -lmysqlclient。可用 SRV_LD=... 覆盖。
MYSQL_LD ?= $(shell mysql_config --libs 2>/dev/null || mariadb_config --libs 2>/dev/null || echo -lmysqlclient)
MYSQL_CF := $(shell mysql_config --cflags 2>/dev/null || mariadb_config --cflags 2>/dev/null)
SRV_LD  := -lpthread $(MYSQL_LD) -lssl -lcrypto

# --- client (C + WebKitGTK) ---
# 自动探测 webkit2gtk 版本 (优先 4.1, 退回 4.0); cJSON 优先 pkg-config 否则 -lcjson
WK_PKG    := $(shell pkg-config --exists webkit2gtk-4.1 && echo webkit2gtk-4.1 || echo webkit2gtk-4.0)
CJSON_CF  := $(shell pkg-config --exists libcjson && pkg-config --cflags libcjson)
CJSON_LD  := $(shell pkg-config --exists libcjson && pkg-config --libs libcjson || echo -lcjson)
CLI_CF    := $(shell pkg-config --cflags gtk+-3.0 $(WK_PKG)) $(CJSON_CF)
CLI_LD    := $(shell pkg-config --libs   gtk+-3.0 $(WK_PKG)) $(CJSON_LD) -lpthread
CLI_SRC   := $(SRC_COMMON) client/main.c

.PHONY: all server client clean
all: server

$(BIN):
	@mkdir -p $(BIN)

server: $(BIN)
	$(CC) $(CFLAGS) $(MYSQL_CF) $(SRV_SRC) -o $(BIN)/chat_server $(SRV_LD)

client: $(BIN)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations $(CLI_CF) $(CLI_SRC) -o $(BIN)/zoo-client $(CLI_LD)

clean:
	rm -rf $(BIN) recv
