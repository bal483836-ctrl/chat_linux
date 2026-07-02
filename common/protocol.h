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

    MSG_FRIEND_ADD     = 20,  /* (兼容) 直接互加, 通常被 FRIEND_REQ 取代 */
    MSG_FRIEND_DEL     = 21,
    MSG_FRIEND_LIST    = 22,  /* body 多行: "name\tonline\tblack"            */
    MSG_BLACK_ADD      = 23,
    MSG_BLACK_DEL      = 24,
    MSG_FRIEND_REQ        = 25,  /* 客户端->服务器: to_name=对方, body=招呼   */
    MSG_FRIEND_REQ_NOTIFY = 26,  /* 服务器->目标: status=reqid, from_name     */
    MSG_FRIEND_REQ_REPLY  = 27,  /* 目标->服务器: status=reqid, group_id=1/0  */
    MSG_FRIEND_REQ_LIST   = 28,  /* 拉取待处理: "reqid\tfrom\ttime\thello"    */
    MSG_FRIEND_REMARK     = 29,  /* 设置好友备注: to_name=对方, body=备注     */

    MSG_GROUP_CREATE   = 30,  /* body=群名                                   */
    MSG_GROUP_JOIN     = 31,  /* (兼容) 直接加群                              */
    MSG_GROUP_LIST     = 32,  /* body 多行: "gid\tname\towner"               */
    MSG_GROUP_MEMBERS  = 33,
    MSG_GROUP_JOIN_REQ        = 34,  /* group_id, body=招呼                   */
    MSG_GROUP_JOIN_NOTIFY     = 35,  /* 服务器->群主: status=reqid, group_id  */
    MSG_GROUP_JOIN_REPLY      = 36,  /* 群主->服务器: status=reqid, group_id=1/0 */
    MSG_GROUP_JOIN_REQ_LIST   = 37,  /* "reqid\tgid\tgname\tfrom\ttime"       */
    MSG_GROUP_LEAVE           = 38,  /* 退出群聊: group_id; 群主不可退出       */
    MSG_GROUP_INVITE          = 39,  /* 成员邀请入群: group_id, body=多行账号 */

    MSG_HISTORY_PRIV   = 40,  /* 拉取私聊历史 to_name=对方                   */
    MSG_HISTORY_GROUP  = 41,  /* 拉取群聊历史 group_id                       */
    MSG_OFFLINE_PULL   = 42,  /* 登录后服务器主动推送                         */

    /* ===== 文件/图片传输 =====
     * 上行(客户端->服务器上传): FILE_BEGIN(to_name=对方 或 group_id=群号,
     *   body="文件名\tMIME", status=总字节数) -> 多个 FILE_CHUNK(body=分片) -> FILE_END。
     *   服务器把分片重组落盘 data/files/<fileid>, 并在 messages 表存一条"文件消息"
     *   (content 以 \x01FILE\t 标记), 走和文本一样的在线推送/离线入队/历史通路。
     * 下行(服务器->客户端下载响应): 同样用 FILE_BEGIN/CHUNK/END, 但 group_id 复用为
     *   fileid, from_name=原发送者账号, 供客户端按 fileid 关联占位消息并组装。*/
    MSG_FILE_BEGIN     = 50,  /* body="文件名\tMIME", status=总字节数           */
    MSG_FILE_CHUNK     = 51,  /* body=二进制分片                              */
    MSG_FILE_END       = 52,
    MSG_FILE_GET       = 53,  /* 客户端请求下载: status=fileid; 服务端回 FILE_BEGIN.. */

    MSG_NOTIFY_ONLINE  = 60,  /* 服务器->客户端: 好友上线                    */
    MSG_NOTIFY_OFFLINE = 61,

    MSG_USER_SEARCH    = 70,  /* body=关键字; 响应: "name\tonline\n..."      */
    MSG_GROUP_SEARCH   = 71,  /* body=关键字; 响应: "gid\tname\towner\tcnt\n"*/

    /* ===== 头像 (PNG 字节) =====
     * 设计要点:
     *   - 上传仅在注册成功后做一次, 也允许登录后随时更新.
     *   - 服务端把字节落到 data/avatars/<id>.png, 没用 BLOB 入库
     *     (BLOB 入 MySQL 要 escape 二进制, 复杂还慢; 文件就够用).
     *   - 64x64 PNG 大小通常 1~3KB, 一次 MSG 装得下;
     *     超过 MAX_BODY_LEN 的图客户端会先 scale_simple 缩到 64.
     */
    MSG_AVATAR_UPLOAD  = 80,  /* 上传自己的头像: body=PNG 字节, status=字节数      */
    MSG_AVATAR_GET     = 81,  /* 拉取头像: to_name=对方账号; 服务端回 MSG_AVATAR_DATA*/
    MSG_AVATAR_DATA    = 82,  /* 服务端回应: from_name=账号, status=字节数, body=PNG*/

    /* ===== 消息检索 ===== (功能 8)
     * 对历史聊天文本做关键字搜索, 包括我参与过的所有私聊和我所在群的群聊.
     * body=关键字; 服务端回同 type, body 每行
     *   "msg_id\ttime\tfrom_nick\tkind\tpeer\tsnippet"
     * 其中 kind 0=私聊 1=群聊; peer 私聊填对方账号、群聊填群号. */
    MSG_MSG_SEARCH     = 90,

    /* ===== 群公告 / 个人资料 (Web 原型新增功能) ===== */
    MSG_GROUP_NOTICE   = 100, /* group_id; body 非空=群主设置, 否则查询;
                              * 服务端回同 type, group_id, body=当前公告      */
    MSG_PROFILE_GET    = 101, /* to_name=账号(空=自己); 回 MSG_PROFILE_DATA   */
    MSG_PROFILE_SET    = 102, /* 更新自己: body="昵称\n出生日期(YYYY-MM-DD)"  */
    MSG_PROFILE_DATA   = 103, /* 回应: from_name=账号,
                              * body="昵称\t出生日期\tavatar_color\tonline"   */
};

/* 应答状态码.
 * 前缀用 RS_ (RespStatus) 而不是 R_, 避免与 <unistd.h> 里的
 * #define R_OK 4 等 POSIX access(2) 宏冲突. */
enum RespStatus {
    RS_OK              = 0,
    RS_FAIL            = 1,
    RS_AUTH_FAIL       = 2,
    RS_USER_EXIST      = 3,
    RS_USER_NOT_FOUND  = 4,
    RS_NOT_FRIEND      = 5,
    RS_IN_BLACKLIST    = 6,
    RS_GROUP_NOT_FOUND = 7,
};

/* 线上消息结构。固定大小, 简化收发.
 *
 * 寻址规则:
 *   from_name  发送者账号 (6 位数字字符串, 由服务器自动生成)
 *   to_name    接收者账号
 *   from_nick  发送者昵称 (服务器填充, 客户端直接显示)
 *
 * 账号生成: account = 100000 + users.id, 第一名用户得到 "100001". */
typedef struct {
    uint32_t type;                       /* enum MsgType                    */
    uint32_t status;                     /* 应答码/文件总大小等            */
    uint32_t group_id;                   /* 群号                            */
    uint32_t body_len;                   /* body 实际字节数 (<= MAX_BODY_LEN)*/
    char     from_name[MAX_NAME_LEN];    /* 发送者账号                      */
    char     to_name[MAX_NAME_LEN];      /* 接收者账号                      */
    char     from_nick[MAX_NAME_LEN];    /* 发送者昵称                      */
    char     timestamp[32];              /* "YYYY-MM-DD HH:MM:SS"           */
    char     body[MAX_BODY_LEN];         /* 文本/二进制载荷                 */
} Message;

#define MSG_SIZE  ((int)sizeof(Message))

#endif
