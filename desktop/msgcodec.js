/* Message 结构编解码 (与 common/protocol.h 一致, 小端, 4240 字节)
 * 供 Electron 主进程直连 C 服务器 TCP 时使用. */
'use strict';
const NAME = 32, BODY = 4096;
const OFF = { type:0, status:4, group_id:8, body_len:12,
              from_name:16, to_name:48, from_nick:80, timestamp:112, body:144 };
const MSG_SIZE = 4240;

function writeCStr(buf, off, len, str){
  buf.fill(0, off, off+len);
  if (str) Buffer.from(String(str), 'utf8').copy(buf, off, 0, len-1);
}
function readCStr(buf, off, len){
  let end = off; const max = off+len;
  while (end < max && buf[end] !== 0) end++;
  return buf.toString('utf8', off, end);
}
function encodeMsg(o){
  const b = Buffer.alloc(MSG_SIZE);
  b.writeUInt32LE((o.type|0)>>>0,     OFF.type);
  b.writeUInt32LE((o.status|0)>>>0,   OFF.status);
  b.writeUInt32LE((o.group_id|0)>>>0, OFF.group_id);
  writeCStr(b, OFF.from_name, NAME, o.from_name);
  writeCStr(b, OFF.to_name,   NAME, o.to_name);
  writeCStr(b, OFF.from_nick, NAME, o.from_nick);
  writeCStr(b, OFF.timestamp, 32,   o.timestamp);
  let payload = (o.bodyB64 != null) ? Buffer.from(o.bodyB64,'base64') : Buffer.from(o.body || '', 'utf8');
  if (payload.length > BODY) payload = payload.slice(0, BODY);
  payload.copy(b, OFF.body);
  b.writeUInt32LE(payload.length>>>0, OFF.body_len);
  return b;
}
function decodeMsg(b){
  const body_len = b.readUInt32LE(OFF.body_len);
  const payload = b.slice(OFF.body, OFF.body + Math.min(body_len, BODY));
  return {
    type:b.readUInt32LE(OFF.type), status:b.readUInt32LE(OFF.status),
    group_id:b.readUInt32LE(OFF.group_id), body_len,
    from_name:readCStr(b,OFF.from_name,NAME), to_name:readCStr(b,OFF.to_name,NAME),
    from_nick:readCStr(b,OFF.from_nick,NAME), timestamp:readCStr(b,OFF.timestamp,32),
    body:payload.toString('utf8'), bodyB64:payload.toString('base64'),
  };
}
module.exports = { encodeMsg, decodeMsg, MSG_SIZE };
