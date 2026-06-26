/* 预加载: 给渲染进程暴露安全的 window.zooNative 接口 (不开放 nodeIntegration) */
'use strict';
const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('zooNative', {
  connect: (host) => ipcRenderer.invoke('zoo:connect', host),
  send:    (obj) => ipcRenderer.send('zoo:send', obj),
  onMsg:   (cb) => ipcRenderer.on('zoo:msg', (_e, obj) => cb(obj)),
  win: {
    minimize:       () => ipcRenderer.send('win:min'),
    toggleMaximize: () => ipcRenderer.send('win:max'),
    close:          () => ipcRenderer.send('win:close'),
  },
});
