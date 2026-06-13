/* =========================================================
 * Linux 程序设计课程设计 - 即时通信系统数据库 schema
 * 使用方法:
 *     mysql -u root -p < sql/init.sql
 * ========================================================= */

CREATE DATABASE IF NOT EXISTS chat_linux
    DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE chat_linux;

/* 用户表 */
CREATE TABLE IF NOT EXISTS users (
    id          INT          NOT NULL AUTO_INCREMENT,
    username    VARCHAR(32)  NOT NULL UNIQUE,
    /* 课程示例: 简单 sha1 即可, 生产环境应使用 bcrypt/argon2 */
    password    VARCHAR(64)  NOT NULL,
    online      TINYINT      NOT NULL DEFAULT 0,
    created_at  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id)
) ENGINE=InnoDB;

/* 好友关系. status: 0 = 普通好友, 1 = 拉黑 */
CREATE TABLE IF NOT EXISTS friends (
    user_id     INT NOT NULL,
    friend_id   INT NOT NULL,
    status      TINYINT NOT NULL DEFAULT 0,
    PRIMARY KEY (user_id, friend_id),
    FOREIGN KEY (user_id)   REFERENCES users(id) ON DELETE CASCADE,
    FOREIGN KEY (friend_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB;

/* 群组 */
CREATE TABLE IF NOT EXISTS chat_groups (
    id          INT NOT NULL AUTO_INCREMENT,
    name        VARCHAR(64) NOT NULL UNIQUE,
    owner_id    INT NOT NULL,
    created_at  DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    FOREIGN KEY (owner_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS group_members (
    group_id  INT NOT NULL,
    user_id   INT NOT NULL,
    PRIMARY KEY (group_id, user_id),
    FOREIGN KEY (group_id) REFERENCES chat_groups(id) ON DELETE CASCADE,
    FOREIGN KEY (user_id)  REFERENCES users(id)       ON DELETE CASCADE
) ENGINE=InnoDB;

/* 消息历史. msg_type: 0=私聊, 1=群聊
 * target_id: 私聊为对方 user_id, 群聊为 group_id */
CREATE TABLE IF NOT EXISTS messages (
    id          BIGINT       NOT NULL AUTO_INCREMENT,
    from_id     INT          NOT NULL,
    target_id   INT          NOT NULL,
    msg_type    TINYINT      NOT NULL,
    content     TEXT         NOT NULL,
    sent_at     DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    INDEX (from_id),
    INDEX (target_id, msg_type)
) ENGINE=InnoDB;

/* 离线消息队列: 用户上线后服务器将这些消息推送给该用户 */
CREATE TABLE IF NOT EXISTS offline_msg (
    id          BIGINT NOT NULL AUTO_INCREMENT,
    user_id     INT    NOT NULL,
    message_id  BIGINT NOT NULL,
    PRIMARY KEY (id),
    INDEX (user_id),
    FOREIGN KEY (user_id)    REFERENCES users(id)    ON DELETE CASCADE,
    FOREIGN KEY (message_id) REFERENCES messages(id) ON DELETE CASCADE
) ENGINE=InnoDB;

/* 好友申请. status: 0=待处理 1=已同意 2=已拒绝 */
CREATE TABLE IF NOT EXISTS friend_requests (
    id          INT NOT NULL AUTO_INCREMENT,
    from_id     INT NOT NULL,
    to_id       INT NOT NULL,
    hello       VARCHAR(256) DEFAULT '',
    status      TINYINT NOT NULL DEFAULT 0,
    created_at  DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    INDEX (to_id, status),
    FOREIGN KEY (from_id) REFERENCES users(id) ON DELETE CASCADE,
    FOREIGN KEY (to_id)   REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB;

/* 入群申请. 由群主审批 */
CREATE TABLE IF NOT EXISTS group_join_requests (
    id          INT NOT NULL AUTO_INCREMENT,
    group_id    INT NOT NULL,
    user_id     INT NOT NULL,
    hello       VARCHAR(256) DEFAULT '',
    status      TINYINT NOT NULL DEFAULT 0,
    created_at  DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (id),
    INDEX (group_id, status),
    FOREIGN KEY (group_id) REFERENCES chat_groups(id) ON DELETE CASCADE,
    FOREIGN KEY (user_id)  REFERENCES users(id)       ON DELETE CASCADE
) ENGINE=InnoDB;
