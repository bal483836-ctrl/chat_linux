# ========================================================
#  chat_linux - 顶层 Makefile
#
#  目标:
#     make server        编译服务器  -> bin/chat_server
#     make client        编译客户端  -> bin/chat_client
#     make               同时编译
#     make clean         清理
#
#  依赖 (Ubuntu/Debian):
#     sudo apt install build-essential libmysqlclient-dev libssl-dev \
#                      libgtk-3-dev pkg-config mysql-server
# ========================================================

CC       ?= gcc
CFLAGS   := -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -Icommon
LDFLAGS  :=

BIN      := bin
SRC_COMMON := common/net_io.c

# --- server ---
SRV_SRC := $(SRC_COMMON) server/server.c server/handler.c server/online.c server/db.c
SRV_LD  := -lpthread -lmysqlclient -lssl -lcrypto

# --- client ---
CLI_SRC := $(SRC_COMMON) client/main.c client/net.c client/ui.c
CLI_PKG := $(shell pkg-config --cflags --libs gtk+-3.0)
CLI_LD  := -lpthread -lm $(CLI_PKG)

.PHONY: all server client clean
all: server client

$(BIN):
	@mkdir -p $(BIN)

server: $(BIN)
	$(CC) $(CFLAGS) $(SRV_SRC) -o $(BIN)/chat_server $(SRV_LD)

client: $(BIN)
	$(CC) $(CFLAGS) $(CLI_SRC) -o $(BIN)/chat_client $(CLI_LD)

clean:
	rm -rf $(BIN) recv
