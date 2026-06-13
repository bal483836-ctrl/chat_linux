#ifndef CHAT_PROTOCOL_H
#define CHAT_PROTOCOL_H

#include <stdint.h>

#define SERVER_PORT       8888
#define MAX_NAME_LEN      32
#define MAX_PASS_LEN      32
#define MAX_BODY_LEN      4096
#define MAX_ONLINE_USERS  1024
#define FILE_CHUNK_SIZE   2048

/* ===== 消息类型 =====
 * 客户端 <-> 服务器之间的所有交互都用同一个 Message 结构。
 * 通过 type 字段区分语义。 */
enum MsgType {
    MSG_REGISTER       = 1,   /* 注册:        body="user\npass"             */
    MSG_LOGIN          = 2,   /* 登录:        body="user\npass"             */
    MSG_LOGOUT         = 3,   /* 退出登录                                    */
    MSG_RESPONSE       = 4,   /* 服务器统一应答, code 在 status, body=描述  */

    MSG_PRIVATE_CHAT   = 10,  /* 私聊: to_name=对方, body=文本               */
    MSG_GROUP_CHAT     = 11,  /* 群聊: group_id=群号, body=文本              */

    MSG_FRIEND_ADD     = 20,
    MSG_FRIEND_DEL     = 21,
    MSG_FRIEND_LIST    = 22,  /* body 多行: "name\tonline\tblack"            */
    MSG_BLACK_ADD      = 23,
    MSG_BLACK_DEL      = 24,

    MSG_GROUP_CREATE   = 30,  /* body=群名                                   */
    MSG_GROUP_JOIN     = 31,  /* group_id                                    */
    MSG_GROUP_LIST     = 32,  /* body 多行: "gid\tname\towner"               */
    MSG_GROUP_MEMBERS  = 33,

    MSG_HISTORY_PRIV   = 40,  /* 拉取私聊历史 to_name=对方                   */
    MSG_HISTORY_GROUP  = 41,  /* 拉取群聊历史 group_id                       */
    MSG_OFFLINE_PULL   = 42,  /* 登录后服务器主动推送                         */

    MSG_FILE_BEGIN     = 50,  /* body=文件名, status=总字节数                */
    MSG_FILE_CHUNK     = 51,  /* body=二进制分片                              */
    MSG_FILE_END       = 52,

    MSG_NOTIFY_ONLINE  = 60,  /* 服务器->客户端: 好友上线                    */
    MSG_NOTIFY_OFFLINE = 61,
};

/* 应答状态码 */
enum RespStatus {
    R_OK              = 0,
    R_FAIL            = 1,
    R_AUTH_FAIL       = 2,
    R_USER_EXIST      = 3,
    R_USER_NOT_FOUND  = 4,
    R_NOT_FRIEND      = 5,
    R_IN_BLACKLIST    = 6,
    R_GROUP_NOT_FOUND = 7,
};

/* 线上消息结构。固定大小, 简化收发. */
typedef struct {
    uint32_t type;                       /* enum MsgType                    */
    uint32_t status;                     /* 应答码/文件总大小等            */
    uint32_t group_id;                   /* 群号                            */
    uint32_t body_len;                   /* body 实际字节数 (<= MAX_BODY_LEN)*/
    char     from_name[MAX_NAME_LEN];    /* 发送者                          */
    char     to_name[MAX_NAME_LEN];      /* 接收者(私聊)                    */
    char     timestamp[32];              /* "YYYY-MM-DD HH:MM:SS"           */
    char     body[MAX_BODY_LEN];         /* 文本/二进制载荷                 */
} Message;

#define MSG_SIZE  ((int)sizeof(Message))

#endif
