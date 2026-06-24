/* ============================================================
 * zoo web bridge  —  浏览器 WebSocket  <->  C 聊天服务器 TCP
 *
 *  浏览器只能用 WebSocket / HTTP, 没法直接连 C 服务器的裸 TCP +
 *  4240 字节定长 Message 结构。这个桥接进程负责:
 *    1) 作为静态服务器, 提供 prototype/ 下的页面与素材
 *    2) 升级 WebSocket, 每个浏览器连接对应一条到 chat_server 的 TCP
 *    3) JSON  <->  Message 结构 互转 (小端, 见 common/protocol.h)
 *
 *  纯 Node 内置模块实现, 无需 npm install。
 *
 *  运行:
 *    node prototype/bridge.js
 *  环境变量:
 *    PORT        桥接 HTTP/WS 端口   (默认 8080)
 *    CHAT_HOST   C 服务器地址        (默认 127.0.0.1)
 *    CHAT_PORT   C 服务器端口        (默认 8888)
 * ============================================================ */
'use strict';
const http = require('http');
const net  = require('net');
const fs   = require('fs');
const path = require('path');
const crypto = require('crypto');

/* ---------- Message 结构编解码 (与 common/protocol.h 完全一致) ---------- */
const NAME = 32, BODY = 4096;
const OFF = { type:0, status:4, group_id:8, body_len:12,
              from_name:16, to_name:48, from_nick:80, timestamp:112, body:144 };
const MSG_SIZE = 4240;

function writeCStr(buf, off, len, str){
  buf.fill(0, off, off+len);
  if (str) Buffer.from(String(str), 'utf8').copy(buf, off, 0, len-1);
}
function readCStr(buf, off, len){
  let end = off;
  const max = off+len;
  while (end < max && buf[end] !== 0) end++;
  return buf.toString('utf8', off, end);
}

/* obj -> 4240B Buffer.  body 默认按 utf8; 若 obj.bodyB64 提供则按二进制. */
function encodeMsg(o){
  const b = Buffer.alloc(MSG_SIZE);
  b.writeUInt32LE((o.type|0)>>>0,     OFF.type);
  b.writeUInt32LE((o.status|0)>>>0,   OFF.status);
  b.writeUInt32LE((o.group_id|0)>>>0, OFF.group_id);
  writeCStr(b, OFF.from_name, NAME, o.from_name);
  writeCStr(b, OFF.to_name,   NAME, o.to_name);
  writeCStr(b, OFF.from_nick, NAME, o.from_nick);
  writeCStr(b, OFF.timestamp, 32,   o.timestamp);
  let payload;
  if (o.bodyB64 != null) payload = Buffer.from(o.bodyB64, 'base64');
  else payload = Buffer.from(o.body || '', 'utf8');
  if (payload.length > BODY) payload = payload.slice(0, BODY);
  payload.copy(b, OFF.body);
  b.writeUInt32LE(payload.length>>>0, OFF.body_len);
  return b;
}

/* 4240B Buffer -> obj.  始终给出 bodyB64; 文本字段额外给 body(utf8). */
function decodeMsg(b){
  const body_len = b.readUInt32LE(OFF.body_len);
  const payload = b.slice(OFF.body, OFF.body + Math.min(body_len, BODY));
  return {
    type:      b.readUInt32LE(OFF.type),
    status:    b.readUInt32LE(OFF.status),
    group_id:  b.readUInt32LE(OFF.group_id),
    body_len,
    from_name: readCStr(b, OFF.from_name, NAME),
    to_name:   readCStr(b, OFF.to_name,   NAME),
    from_nick: readCStr(b, OFF.from_nick, NAME),
    timestamp: readCStr(b, OFF.timestamp, 32),
    body:      payload.toString('utf8'),
    bodyB64:   payload.toString('base64'),
  };
}

/* ---------- 极简 WebSocket (RFC6455) 服务端 ---------- */
const WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
function wsAccept(key){ return crypto.createHash('sha1').update(key+WS_GUID).digest('base64'); }

function wsEncode(str){
  const data = Buffer.from(str, 'utf8');
  const len = data.length;
  let header;
  if (len < 126){ header = Buffer.alloc(2); header[1] = len; }
  else if (len < 65536){ header = Buffer.alloc(4); header[1] = 126; header.writeUInt16BE(len, 2); }
  else { header = Buffer.alloc(10); header[1] = 127; header.writeUInt32BE(0,2); header.writeUInt32BE(len,6); }
  header[0] = 0x81; // FIN + text
  return Buffer.concat([header, data]);
}
function wsClose(){ return Buffer.from([0x88,0x00]); }

/* 解析客户端帧, 回调每个完整文本消息. 返回处理后剩余 buffer. */
function wsDecodeStream(buf, onText, onClose){
  while (buf.length >= 2){
    const op = buf[0] & 0x0f;
    const masked = (buf[1] & 0x80) !== 0;
    let len = buf[1] & 0x7f;
    let off = 2;
    if (len === 126){ if (buf.length < 4) break; len = buf.readUInt16BE(2); off = 4; }
    else if (len === 127){ if (buf.length < 10) break; len = Number(buf.readBigUInt64BE(2)); off = 10; }
    const need = off + (masked?4:0) + len;
    if (buf.length < need) break;
    let mask = null;
    if (masked){ mask = buf.slice(off, off+4); off += 4; }
    const payload = buf.slice(off, off+len);
    if (mask) for (let i=0;i<payload.length;i++) payload[i] ^= mask[i&3];
    buf = buf.slice(need);
    if (op === 0x8){ onClose(); return buf; }
    if (op === 0x1) onText(payload.toString('utf8'));
    // op 0x9 ping / 0xA pong / 0x0 continuation: 原型里忽略
  }
  return buf;
}

/* ---------- 静态文件服务 ---------- */
const ROOT = __dirname;
const MIME = {'.html':'text/html; charset=utf-8','.js':'text/javascript','.css':'text/css',
  '.png':'image/png','.jpg':'image/jpeg','.svg':'image/svg+xml','.json':'application/json','.glb':'model/gltf-binary'};
function serveStatic(req, res){
  let p = decodeURIComponent(req.url.split('?')[0]);
  if (p === '/' ) p = '/zoo-chat.html';
  const file = path.normalize(path.join(ROOT, p));
  if (!file.startsWith(ROOT)){ res.writeHead(403); res.end('forbidden'); return; }
  fs.readFile(file, (err, data)=>{
    if (err){ res.writeHead(404); res.end('not found'); return; }
    res.writeHead(200, {'Content-Type': MIME[path.extname(file)] || 'application/octet-stream'});
    res.end(data);
  });
}

/* ---------- 主服务 ---------- */
const PORT      = process.env.PORT      || 8080;
const CHAT_HOST = process.env.CHAT_HOST || '127.0.0.1';
const CHAT_PORT = process.env.CHAT_PORT || 8888;

const server = http.createServer(serveStatic);

server.on('upgrade', (req, socket)=>{
  if (req.headers['upgrade'] !== 'websocket'){ socket.destroy(); return; }
  const key = req.headers['sec-websocket-key'];
  socket.write(
    'HTTP/1.1 101 Switching Protocols\r\n'+
    'Upgrade: websocket\r\nConnection: Upgrade\r\n'+
    'Sec-WebSocket-Accept: '+wsAccept(key)+'\r\n\r\n');

  // 每个 WS 连接 -> 一条到 C 服务器的 TCP
  const tcp = net.connect(CHAT_PORT, CHAT_HOST);
  let tcpBuf = Buffer.alloc(0);
  let wsBuf  = Buffer.alloc(0);
  let alive = true;
  const sendWS = (obj)=>{ if (alive) try { socket.write(wsEncode(JSON.stringify(obj))); } catch(e){} };

  tcp.on('connect', ()=> sendWS({type:0, _ev:'bridge', ok:true, msg:'connected to chat_server'}));
  tcp.on('data', (chunk)=>{
    tcpBuf = Buffer.concat([tcpBuf, chunk]);
    while (tcpBuf.length >= MSG_SIZE){
      const frame = tcpBuf.slice(0, MSG_SIZE);
      tcpBuf = tcpBuf.slice(MSG_SIZE);
      sendWS(decodeMsg(frame));
    }
  });
  tcp.on('error', (e)=> sendWS({type:0,_ev:'bridge',ok:false,msg:'tcp error: '+e.message}));
  tcp.on('close', ()=>{ if (alive){ try{ socket.write(wsClose()); }catch(e){} socket.destroy(); } });

  socket.on('data', (chunk)=>{
    wsBuf = Buffer.concat([wsBuf, chunk]);
    wsBuf = wsDecodeStream(wsBuf,
      (text)=>{ try { const o = JSON.parse(text); tcp.write(encodeMsg(o)); } catch(e){} },
      ()=>{ alive=false; tcp.end(); });
  });
  socket.on('close', ()=>{ alive=false; tcp.end(); });
  socket.on('error', ()=>{ alive=false; tcp.end(); });
});

if (require.main === module){
  server.listen(PORT, ()=>{
    console.log(`[zoo-bridge] http+ws  :${PORT}`);
    console.log(`[zoo-bridge] proxy -> chat_server ${CHAT_HOST}:${CHAT_PORT}`);
    console.log(`[zoo-bridge] open    http://localhost:${PORT}/`);
  });
}

module.exports = { encodeMsg, decodeMsg, MSG_SIZE, wsEncode, wsAccept };
