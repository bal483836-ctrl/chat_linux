/* zoo 后端全功能集成测试(多客户端). 需先启动 chat_server(:8888)+MySQL
 * 运行:  node prototype/test_full.js
 * 覆盖: 注册/登录/错误密码/好友申请/私聊/历史(to_name)/离线消息/备注/
 *       建群/邀请/群成员/群聊/群公告/资料/搜索/上下线通知/拉黑/删好友 */
const net=require('net');
const {encodeMsg,decodeMsg,MSG_SIZE}=require('./bridge.js');
const T={REGISTER:1,LOGIN:2,LOGOUT:3,RESPONSE:4,PRIVATE_CHAT:10,GROUP_CHAT:11,
  FRIEND_DEL:21,FRIEND_LIST:22,BLACK_ADD:23,FRIEND_REQ:25,FRIEND_REQ_NOTIFY:26,FRIEND_REQ_REPLY:27,
  GROUP_CREATE:30,GROUP_LIST:32,GROUP_MEMBERS:33,GROUP_INVITE:39,HISTORY_PRIV:40,HISTORY_GROUP:41,
  NOTIFY_ONLINE:60,NOTIFY_OFFLINE:61,USER_SEARCH:70,GROUP_SEARCH:71,MSG_SEARCH:90,
  GROUP_NOTICE:100,PROFILE_GET:101,PROFILE_SET:102,PROFILE_DATA:103};
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
let PASS=0,FAIL=0; const chk=(n,ok,ex)=>{console.log((ok?'  ✅ ':'  ❌ ')+n+(ex!==undefined&&!ok?'   << '+ex:''));ok?PASS++:FAIL++;};
class Client{
  constructor(tag){this.tag=tag;this.buf=Buffer.alloc(0);this.inbox=[];this.respQ=[];this.waiters=[];this.acc=null;this.nick=null;}
  connect(){return new Promise((res,rej)=>{this.s=net.connect(8888,'127.0.0.1');
    this.s.on('connect',()=>res());this.s.on('error',rej);
    this.s.on('data',c=>{this.buf=Buffer.concat([this.buf,c]);while(this.buf.length>=MSG_SIZE){const m=decodeMsg(this.buf.slice(0,MSG_SIZE));this.buf=this.buf.slice(MSG_SIZE);this._on(m);}});});}
  _on(m){ if(m.type===T.RESPONSE&&this.respQ.length){this.respQ.shift()(m);return;}
    const i=this.waiters.findIndex(w=>w.p(m)); if(i>=0){const w=this.waiters[i];this.waiters.splice(i,1);clearTimeout(w.t);w.r(m);return;} this.inbox.push(m); }
  send(o){this.s.write(encodeMsg(o));}
  req(o){return new Promise(r=>{this.respQ.push(r);this.send(o);});}
  wait(p,d,to=2500){const i=this.inbox.findIndex(p);if(i>=0){const m=this.inbox[i];this.inbox.splice(i,1);return Promise.resolve(m);}
    return new Promise((r,j)=>{const w={p,r};w.t=setTimeout(()=>{this.waiters=this.waiters.filter(x=>x!==w);j(new Error('timeout: '+d+' ['+this.tag+']'));},to);this.waiters.push(w);});}
  drain(type){this.inbox=this.inbox.filter(x=>x.type!==type);}
  close(){return new Promise(r=>{this.s.end(()=>r());this.s.destroy();setTimeout(r,100);});}
  async register(nick,pw){const r=await this.req({type:T.REGISTER,body:nick+'\n'+pw});this.nick=nick;return r.body.trim();}
  async login(acc,pw){const r=await this.req({type:T.LOGIN,body:acc+'\n'+pw});this.acc=acc;return r;}
}
const F=l=>l.split('\n').filter(Boolean);
const rnd=()=>Math.floor(Math.random()*9000+1000);

(async()=>{
 try{
  // ===== 注册 =====
  console.log('\n[1] 注册三个用户');
  const A=new Client('A'),B=new Client('B'),C=new Client('C');
  await A.connect();await B.connect();await C.connect();
  const an='甲'+rnd(),bn='乙'+rnd(),cn='丙'+rnd();
  const aAcc=await A.register(an,'pw123456');
  const bAcc=await B.register(bn,'pw123456');
  const cAcc=await C.register(cn,'pw123456');
  chk('A 注册', /^\d{6,}$/.test(aAcc), aAcc);
  chk('B 注册', /^\d{6,}$/.test(bAcc), bAcc);
  chk('C 注册', /^\d{6,}$/.test(cAcc), cAcc);

  // ===== 错误密码 =====
  console.log('\n[2] 错误密码登录被拒');
  const r=await A.req({type:T.LOGIN,body:aAcc+'\nWRONG'});
  chk('错误密码 -> AUTH_FAIL(2)', r.status===2, 'status='+r.status);

  // ===== 登录 A,B (C 先不登录, 留作离线测试) =====
  console.log('\n[3] A、B 登录(C 暂不登录)');
  const la=await A.login(aAcc,'pw123456'); chk('A 登录', la.status===0, JSON.stringify(la.body));
  const lb=await B.login(bAcc,'pw123456'); chk('B 登录', lb.status===0);
  await sleep(150); A.drain(T.FRIEND_LIST);B.drain(T.FRIEND_LIST);A.drain(T.GROUP_LIST);B.drain(T.GROUP_LIST);

  // ===== 好友申请流程 =====
  console.log('\n[4] 好友申请 A->B, B 同意');
  const fr=await A.req({type:T.FRIEND_REQ,to_name:bAcc,body:'交个朋友'}); chk('A 发送申请', fr.status===0, fr.body);
  const notify=await B.wait(m=>m.type===T.FRIEND_REQ_NOTIFY,'B 收到好友申请通知');
  chk('B 收到 FRIEND_REQ_NOTIFY', notify.from_name===aAcc, notify.from_name);
  const reqid=notify.status;
  const rep=await B.req({type:T.FRIEND_REQ_REPLY,status:reqid,group_id:1}); chk('B 同意', rep.status===0, rep.body);
  await sleep(200);
  // A、B 各自重新拉好友列表确认
  A.drain(T.FRIEND_LIST); A.send({type:T.FRIEND_LIST}); const afl=await A.wait(m=>m.type===T.FRIEND_LIST,'A 好友列表');
  chk('A 好友列表含 B', F(afl.body).some(l=>l.startsWith(bAcc)), afl.body.replace(/\n/g,' | '));
  B.drain(T.FRIEND_LIST); B.send({type:T.FRIEND_LIST}); const bfl=await B.wait(m=>m.type===T.FRIEND_LIST,'B 好友列表');
  chk('B 好友列表含 A', F(bfl.body).some(l=>l.startsWith(aAcc)));

  // ===== 私聊(在线) =====
  console.log('\n[5] 私聊 A->B(在线即时收到)');
  A.send({type:T.PRIVATE_CHAT,to_name:bAcc,body:'你好乙 🐾'});
  const pm=await B.wait(m=>m.type===T.PRIVATE_CHAT&&m.body.includes('你好乙'),'B 收到私聊');
  chk('B 即时收到私聊, from=A', pm.from_name===aAcc, pm.from_name);

  // ===== 历史(带 to_name) =====
  console.log('\n[6] 私聊历史 A<->B 带 to_name');
  A.drain(T.PRIVATE_CHAT); A.send({type:T.HISTORY_PRIV,to_name:bAcc}); await sleep(250);
  const hist=A.inbox.filter(x=>x.type===T.PRIVATE_CHAT);
  const mine=hist.find(x=>x.from_name===aAcc);
  chk('A 拉到历史', hist.length>=1, 'count='+hist.length);
  chk('我发的历史 to_name=B(修复验证)', mine&&mine.to_name===bAcc, mine&&JSON.stringify(mine.to_name));
  A.drain(T.PRIVATE_CHAT);

  // ===== 离线消息: A->C, C 登录后收到 =====
  console.log('\n[7] 离线消息 A->C, C 登录后收到');
  A.send({type:T.PRIVATE_CHAT,to_name:cAcc,body:'离线给丙的消息 📮'});
  await sleep(150);
  const lc=await C.login(cAcc,'pw123456'); chk('C 登录', lc.status===0);
  const offline=await C.wait(m=>m.type===T.PRIVATE_CHAT&&m.body.includes('离线给丙'),'C 收到离线消息',3000);
  chk('C 登录后收到离线消息', offline.from_name===aAcc, offline.from_name);
  await sleep(150); C.drain(T.FRIEND_LIST);C.drain(T.GROUP_LIST);

  // ===== 好友备注 =====
  console.log('\n[8] 好友备注 A 给 B');
  await A.req({type:T.FRIEND_REQ===0?0:29,to_name:bAcc,body:'我的好基友'}); // FRIEND_REMARK=29
  A.drain(T.FRIEND_LIST); A.send({type:T.FRIEND_LIST}); const afl2=await A.wait(m=>m.type===T.FRIEND_LIST,'A 好友列表2');
  const bline=F(afl2.body).find(l=>l.startsWith(bAcc));
  chk('B 这行有备注列=我的好基友', (bline.split('\t')[5]||'')==='我的好基友', JSON.stringify(bline));

  // ===== 建群 + 邀请 + 成员 =====
  console.log('\n[9] A 建群, 邀请 B、C, 群成员');
  const gc=await A.req({type:T.GROUP_CREATE,body:'派对车队'+rnd()}); const gid=parseInt(gc.body,10);
  chk('建群', gid>0, 'gid='+gid);
  B.drain(T.GROUP_LIST); C.drain(T.GROUP_LIST);
  const inv=await A.req({type:T.GROUP_INVITE,group_id:gid,body:bAcc+'\n'+cAcc});
  chk('邀请应答(2人)', /已邀请\s*2/.test(inv.body), inv.body);
  const bgl=await B.wait(m=>m.type===T.GROUP_LIST,'B 收到群列表推送');
  chk('B 被拉群后收到群列表', F(bgl.body).some(l=>l.startsWith(gid+'\t')||l.startsWith(String(gid))), bgl.body.replace(/\n/g,'|'));
  A.send({type:T.GROUP_MEMBERS,group_id:gid}); const gm=await A.wait(m=>m.type===T.GROUP_MEMBERS,'群成员');
  const mem=F(gm.body).map(l=>l.split('\t')[0]);
  chk('群成员含 A/B/C', mem.includes(aAcc)&&mem.includes(bAcc)&&mem.includes(cAcc), JSON.stringify(mem));

  // ===== 群聊(在线) =====
  console.log('\n[10] 群聊 A-> 群, B 即时收到');
  A.send({type:T.GROUP_CHAT,group_id:gid,body:'集合了兄弟们 🎮'});
  const gmsg=await B.wait(m=>m.type===T.GROUP_CHAT&&m.body.includes('集合了'),'B 收到群消息');
  chk('B 收到群消息 from=A', gmsg.from_name===aAcc&&gmsg.group_id===gid, gmsg.from_name+'/'+gmsg.group_id);

  // ===== 群公告 =====
  console.log('\n[11] 群公告(群主设置/非群主拒绝/查询)');
  A.send({type:T.GROUP_NOTICE,group_id:gid,body:'每晚八点准时 🐾'});
  const n1=await A.wait(m=>m.type===T.GROUP_NOTICE,'A 设公告');
  chk('群主设置公告成功', n1.body==='每晚八点准时 🐾', JSON.stringify(n1.body));
  const nb=await B.req({type:T.GROUP_NOTICE,group_id:gid,body:'我想改'}); // 非群主设置 -> 走 req 等 RESPONSE? 服务器 fail 时回 RESPONSE
  chk('非群主改公告被拒(RS_FAIL)', nb.status===1, 'status='+nb.status+' body='+nb.body);
  B.send({type:T.GROUP_NOTICE,group_id:gid,body:''}); const n2=await B.wait(m=>m.type===T.GROUP_NOTICE,'B 查询公告');
  chk('B 查询到公告(未被改动)', n2.body==='每晚八点准时 🐾', JSON.stringify(n2.body));

  // ===== 个人资料 =====
  console.log('\n[12] 个人资料 设置/查询');
  await A.req({type:T.PROFILE_SET,body:'甲改名\n1995-05-05'});
  A.send({type:T.PROFILE_GET,to_name:''}); const pd=await A.wait(m=>m.type===T.PROFILE_DATA,'A 自己资料');
  const pf=pd.body.split('\t');
  chk('A 昵称已改', pf[0]==='甲改名', pf[0]); chk('A 生日已存', pf[1]==='1995-05-05', pf[1]);
  B.send({type:T.PROFILE_GET,to_name:aAcc}); const pdb=await B.wait(m=>m.type===T.PROFILE_DATA,'B 查 A 资料');
  chk('B 能查到 A 的资料', pdb.from_name===aAcc&&pdb.body.split('\t')[0]==='甲改名', pdb.body);

  // ===== 搜索 =====
  console.log('\n[13] 搜索(用户/群/消息)');
  A.send({type:T.USER_SEARCH,body:'小助手'}); const us=await A.wait(m=>m.type===T.USER_SEARCH,'用户搜索');
  chk('用户搜索"小助手"有结果', us.body.includes('小助手'), JSON.stringify(us.body));
  A.send({type:T.GROUP_SEARCH,body:'派对车队'}); const gs=await A.wait(m=>m.type===T.GROUP_SEARCH,'群搜索');
  chk('群搜索"派对车队"有结果', /派对车队/.test(gs.body), JSON.stringify(gs.body).slice(0,80));
  A.send({type:T.MSG_SEARCH,body:'集合了'}); const ms=await A.wait(m=>m.type===T.MSG_SEARCH,'消息搜索');
  chk('消息搜索"集合了"命中', ms.body.includes('集合了'), JSON.stringify(ms.body).slice(0,100));

  // ===== 上下线通知 =====
  console.log('\n[14] 上下线通知(B 断开 -> A 收到离线; B 重连登录 -> A 收到上线)');
  A.drain(T.NOTIFY_OFFLINE);A.drain(T.NOTIFY_ONLINE);
  await B.close();
  const off=await A.wait(m=>m.type===T.NOTIFY_OFFLINE&&m.from_name===bAcc,'A 收到 B 离线',3000).catch(()=>null);
  chk('A 收到 B 的下线通知', !!off, off?'':'未收到');
  const B2=new Client('B2'); await B2.connect(); await B2.login(bAcc,'pw123456');
  const on=await A.wait(m=>m.type===T.NOTIFY_ONLINE&&m.from_name===bAcc,'A 收到 B 上线',3000).catch(()=>null);
  chk('A 收到 B 的上线通知', !!on, on?'':'未收到');

  // ===== 拉黑 =====
  console.log('\n[15] 拉黑: A 拉黑 B, B 再私聊 A 被拒');
  await A.req({type:T.BLACK_ADD,to_name:bAcc});
  const blk=await B2.req({type:T.PRIVATE_CHAT,to_name:aAcc,body:'还能发吗'});
  chk('被拉黑后私聊返回 IN_BLACKLIST(6)', blk.status===6, 'status='+blk.status+' '+blk.body);

  // ===== 删除好友 =====
  console.log('\n[16] 删除好友: A 删除 B');
  await A.req({type:T.FRIEND_DEL,to_name:bAcc});
  A.drain(T.FRIEND_LIST); A.send({type:T.FRIEND_LIST}); const afl3=await A.wait(m=>m.type===T.FRIEND_LIST,'A 好友列表3');
  chk('A 好友列表不再含 B', !F(afl3.body).some(l=>l.startsWith(bAcc)), afl3.body.replace(/\n/g,'|'));

  console.log('\n=================== 结果 ===================');
  console.log('  PASS='+PASS+'  FAIL='+FAIL);
  console.log(FAIL===0?'  🎉 全部通过':'  ⚠️ 有失败项, 见上 ❌');
  process.exit(FAIL===0?0:1);
 }catch(e){console.log('\n测试异常:',e.message);process.exit(2);}
})();
