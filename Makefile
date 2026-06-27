# ========================================================
#  zoo (chat_linux) - 顶层 Makefile
#
#  目标:
#     make server        编译服务器  -> bin/chat_server
#     make               同 server
#     make clean         清理
#
#  桌面客户端在 desktop/ (Electron), 见 desktop/README.md。
#
#  依赖 (Ubuntu/Debian):
#     sudo apt install build-essential libmysqlclient-dev libssl-dev \
#                      pkg-config mysql-server
# ========================================================

CC       ?= gcc
CFLAGS   := -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -Icommon
LDFLAGS  :=

BIN      := bin
SRC_COMMON := common/net_io.c

# --- server ---
SRV_SRC := $(SRC_COMMON) server/server.c server/handler.c server/online.c server/db.c
SRV_LD  := -lpthread -lmysqlclient -lssl -lcrypto

.PHONY: all server clean
all: server

$(BIN):
	@mkdir -p $(BIN)

server: $(BIN)
	$(CC) $(CFLAGS) $(SRV_SRC) -o $(BIN)/chat_server $(SRV_LD)

clean:
	rm -rf $(BIN) recv
