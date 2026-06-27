/* ============================================================
 * zoo-net.js  —  浏览器端网络层 (连接 bridge.js -> C 聊天服务器)
 *
 *  - 通过 WebSocket 连到 bridge, 收发 JSON; bridge 负责与 4240B
 *    Message 结构互转。
 *  - 纯逻辑, 不碰 DOM。UI 通过 on(type, fn) 订阅服务器推送。
 *  - 需要应答的请求 (登录/注册/建群/好友申请) 用 FIFO 解析队列等待
 *    下一条 MSG_RESPONSE (与服务器逐条应答的模型一致)。
 * ============================================================ */
(function(global){
'use strict';

const T = {
  REGISTER:1, LOGIN:2, LOGOUT:3, RESPONSE:4,
  PRIVATE_CHAT:10, GROUP_CHAT:11,
  FRIEND_DEL:21, FRIEND_LIST:22, BLACK_ADD:23, BLACK_DEL:24,
  FRIEND_REQ:25, FRIEND_REQ_NOTIFY:26, FRIEND_REQ_REPLY:27, FRIEND_REQ_LIST:28, FRIEND_REMARK:29,
  GROUP_CREATE:30, GROUP_LIST:32, GROUP_MEMBERS:33,
  GROUP_JOIN_REQ:34, GROUP_JOIN_NOTIFY:35, GROUP_JOIN_REPLY:36, GROUP_JOIN_REQ_LIST:37, GROUP_INVITE:39,
  HISTORY_PRIV:40, HISTORY_GROUP:41, OFFLINE_PULL:42,
  FILE_BEGIN:50, FILE_CHUNK:51, FILE_END:52,
  NOTIFY_ONLINE:60, NOTIFY_OFFLINE:61,
  USER_SEARCH:70, GROUP_SEARCH:71,
  AVATAR_UPLOAD:80, AVATAR_GET:81, AVATAR_DATA:82, MSG_SEARCH:90,
  GROUP_NOTICE:100, PROFILE_GET:101, PROFILE_SET:102, PROFILE_DATA:103,
};
const RS = { OK:0, FAIL:1, AUTH_FAIL:2, USER_EXIST:3, USER_NOT_FOUND:4, NOT_FRIEND:5, IN_BLACKLIST:6, GROUP_NOT_FOUND:7 };

class ZooNet {
  constructor(url){
    this.url = url;
    this.ws = null;
    this.handlers = {};     // type -> [fn]
    this.respQ = [];        // 等待 RESPONSE 的 resolver
    this.connected = false;
  }
  on(type, fn){ (this.handlers[type] = this.handlers[type] || []).push(fn); return this; }
  _emit(type, msg){ (this.handlers[type]||[]).forEach(fn=>{ try{ fn(msg); }catch(e){ console.error(e); } });
                    (this.handlers['*']||[]).forEach(fn=>{ try{ fn(msg); }catch(e){} }); }

  /* 统一分发: 桥接事件 / RESPONSE 应答队列 / 普通推送 */
  _dispatch(m){
    if (m._ev === 'bridge'){ if (this._bridge) this._bridge(m); return; }
    if (m.type === T.RESPONSE && this.respQ.length){ this.respQ.shift()(m); return; }
    this._emit(m.type, m);
  }

  /* 两种传输:
   *   - 桌面版(Electron): window.zooNative(主进程直连 C 服务器 TCP)
   *   - 浏览器版: WebSocket -> bridge.js -> TCP */
  connect(host){
    const native = (typeof window !== 'undefined' && window.zooNative) ? window.zooNative : null;
    return new Promise((resolve, reject)=>{
      let settled = false;
      this._bridge = (m)=>{
        if (m.ok){ this.connected = true; if(!settled){settled=true; resolve(this);} }
        else { this._emit('error', m); if(!settled){settled=true; reject(new Error(m.msg));} }
      };
      if (native){
        this.native = native;
        if (!this._nativeBound){ native.onMsg(m=>this._dispatch(m)); this._nativeBound = true; }
        Promise.resolve(native.connect(host)).catch(e=>{ if(!settled){settled=true; reject(e);} });
      } else {
        try { this.ws = new WebSocket(this.url); } catch(e){ return reject(e); }
        this.ws.onmessage = (ev)=>{ let m; try { m = JSON.parse(ev.data); } catch(e){ return; } this._dispatch(m); };
        this.ws.onerror = (e)=>{ this._emit('error', e); if(!settled){settled=true; reject(e);} };
        this.ws.onclose = ()=>{ this.connected = false; this._emit('close', {}); };
      }
      setTimeout(()=>{ if(!settled){settled=true; reject(new Error('connect timeout')); } }, 6000);
    });
  }
  send(obj){ if (this.native){ this.native.send(obj); return; }
             if (this.ws && this.ws.readyState === 1) this.ws.send(JSON.stringify(obj)); }
  _req(obj){ return new Promise(res=>{ this.respQ.push(res); this.send(obj); }); }

  /* ---- 账号 ---- */
  register(nick, pass, email){ return this._req({type:T.REGISTER, body: nick + '\n' + pass + (email ? ('\n' + email) : '')}); }
  login(account, pass){ return this._req({type:T.LOGIN,   body: account + '\n' + pass}); }
  logout(){ this.send({type:T.LOGOUT}); }

  /* ---- 聊天 ---- */
  sendPrivate(toAccount, text){ this.send({type:T.PRIVATE_CHAT, to_name:String(toAccount), body:text}); }
  sendGroup(gid, text){ this.send({type:T.GROUP_CHAT, group_id:Number(gid), body:text}); }
  historyPriv(acc){ this.send({type:T.HISTORY_PRIV, to_name:String(acc)}); }
  historyGroup(gid){ this.send({type:T.HISTORY_GROUP, group_id:Number(gid)}); }

  /* ---- 好友 / 群 ---- */
  friendList(){ this.send({type:T.FRIEND_LIST}); }
  friendReq(acc, hello){ return this._req({type:T.FRIEND_REQ, to_name:String(acc), body:hello||'交个朋友吧~'}); }
  friendReqReply(reqid, ok){ this.send({type:T.FRIEND_REQ_REPLY, status:Number(reqid), group_id: ok?1:0}); }
  friendDel(acc){ return this._req({type:T.FRIEND_DEL, to_name:String(acc)}); }
  blackAdd(acc){ return this._req({type:T.BLACK_ADD, to_name:String(acc)}); }
  friendRemark(acc, remark){ return this._req({type:T.FRIEND_REMARK, to_name:String(acc), body:remark||''}); }
  groupList(){ this.send({type:T.GROUP_LIST}); }
  groupCreate(name){ return this._req({type:T.GROUP_CREATE, body:name}); }
  groupMembers(gid){ this.send({type:T.GROUP_MEMBERS, group_id:Number(gid)}); }
  groupInvite(gid, accounts){ return this._req({type:T.GROUP_INVITE, group_id:Number(gid), body:(accounts||[]).join('\n')}); }
  groupNotice(gid, text){ this.send({type:T.GROUP_NOTICE, group_id:Number(gid), body:text||''}); }

  /* ---- 个人资料 ---- */
  profileGet(acc){ this.send({type:T.PROFILE_GET, to_name: acc?String(acc):''}); }
  profileSet(nick, birth){ return this._req({type:T.PROFILE_SET, body:(nick||'')+'\n'+(birth||'')}); }

  /* ---- 检索 ---- */
  userSearch(kw){ this.send({type:T.USER_SEARCH, body:kw}); }
  groupSearch(kw){ this.send({type:T.GROUP_SEARCH, body:kw}); }
  msgSearch(kw){ this.send({type:T.MSG_SEARCH, body:kw}); }

  /* ---- 解析服务器多行文本载荷 ---- */
  static parseFriendList(body){ return splitLines(body).map(l=>{ const p=l.split('\t');
    return { account:p[0], nick:p[1], color:+p[2]||0, online:+p[3]===1, black:+p[4]===1, remark:p[5]||'' }; }); }
  static parseGroupList(body){ return splitLines(body).map(l=>{ const p=l.split('\t');
    return { gid:+p[0], name:p[1], owner:p[2] }; }); }
  static parseFreqList(body){ return splitLines(body).map(l=>{ const p=l.split('\t');
    return { reqid:+p[0], account:p[1], nick:p[2], color:+p[3]||0, time:p[4], hello:p[5]||'' }; }); }
  static parseMembers(body){ return splitLines(body).map(l=>{ const p=l.split('\t');
    return { account:p[0], nick:p[1], color:+p[2]||0, online:+p[3]===1 }; }); }
  static parseSearch(body){ return splitLines(body).map(l=>{ const p=l.split('\t');
    return { msgId:p[0], time:p[1], fromNick:p[2], kind:+p[3], peer:p[4], snippet:p[5] }; }); }
  static parseProfile(body){ const p=(body||'').split('\t');
    return { nick:p[0]||'', birth:p[1]||'', color:+p[2]||0, online:+p[3]===1 }; }
}
function splitLines(s){ return (s||'').split('\n').map(x=>x.trim()).filter(Boolean); }

ZooNet.T = T; ZooNet.RS = RS;
global.ZooNet = ZooNet;
})(window);
