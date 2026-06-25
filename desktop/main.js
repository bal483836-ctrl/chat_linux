/* ============================================================
 * zoo 桌面版 (Electron) - 主进程
 *
 *  - 创建独立桌面窗口, 加载 prototype/zoo-chat.html
 *  - 主进程(Node)直接用 TCP 连 C 聊天服务器, 不需要浏览器/桥接/localhost
 *  - 与渲染进程用 IPC 收发 JSON; JSON <-> 4240B Message 结构 互转
 *
 *  环境变量: CHAT_HOST(默认127.0.0.1) / CHAT_PORT(默认8888)
 * ============================================================ */
'use strict';
const { app, BrowserWindow, ipcMain, Menu } = require('electron');
const net  = require('net');
const path = require('path');
const { encodeMsg, decodeMsg, MSG_SIZE } = require('./msgcodec');

const CHAT_HOST = process.env.CHAT_HOST || '127.0.0.1';
const CHAT_PORT = Number(process.env.CHAT_PORT || 8888);

let win = null, tcp = null, tcpBuf = Buffer.alloc(0);

function protoDir(){
  return app.isPackaged ? path.join(process.resourcesPath, 'prototype')
                        : path.join(__dirname, '..', 'prototype');
}
function toRenderer(obj){ if (win && !win.isDestroyed()) win.webContents.send('zoo:msg', obj); }

function connectTCP(){
  if (tcp && !tcp.destroyed) return;
  tcpBuf = Buffer.alloc(0);
  tcp = net.connect(CHAT_PORT, CHAT_HOST);
  tcp.on('connect', ()=> toRenderer({ type:0, _ev:'bridge', ok:true,  msg:`connected ${CHAT_HOST}:${CHAT_PORT}` }));
  tcp.on('data', (chunk)=>{
    tcpBuf = Buffer.concat([tcpBuf, chunk]);
    while (tcpBuf.length >= MSG_SIZE){
      const frame = tcpBuf.slice(0, MSG_SIZE);
      tcpBuf = tcpBuf.slice(MSG_SIZE);
      toRenderer(decodeMsg(frame));
    }
  });
  tcp.on('error', (e)=> toRenderer({ type:0, _ev:'bridge', ok:false, msg:'tcp error: '+e.message }));
  tcp.on('close', ()=> toRenderer({ type:0, _ev:'bridge', ok:false, msg:'connection closed' }));
}

ipcMain.handle('zoo:connect', ()=>{ connectTCP(); return true; });
ipcMain.on('zoo:send', (_e, obj)=>{ if (tcp && !tcp.destroyed) { try { tcp.write(encodeMsg(obj)); } catch(e){} } });

function createWindow(){
  win = new BrowserWindow({
    width: 1200, height: 800, minWidth: 940, minHeight: 600,
    title: 'zoo', backgroundColor: '#f5942e', autoHideMenuBar: true, show: false,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true, nodeIntegration: false,
    },
  });
  Menu.setApplicationMenu(null);
  win.loadFile(path.join(protoDir(), 'zoo-chat.html'));
  win.once('ready-to-show', ()=> win.show());
  win.on('closed', ()=>{ win = null; });
}

app.whenReady().then(()=>{
  createWindow();
  app.on('activate', ()=>{ if (BrowserWindow.getAllWindows().length === 0) createWindow(); });
});
app.on('window-all-closed', ()=>{ if (tcp) tcp.destroy(); if (process.platform !== 'darwin') app.quit(); });
