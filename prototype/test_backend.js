/* zoo 后端集成测试: 需先启动 chat_server(:8888) + MySQL
 * 运行:  node prototype/test_backend.js
 * 覆盖: 注册/登录/资料/好友备注/建群/邀请入群/群成员/群公告 */
const net=require('net');
const {encodeMsg,decodeMsg,MSG_SIZE}=require('../desktop/msgcodec');
const T={REGISTER:1,LOGIN:2,RESPONSE:4,FRIEND_LIST:22,FRIEND_REMARK:29,GROUP_CREATE:30,GROUP_LIST:32,
  GROUP_MEMBERS:33,GROUP_INVITE:39,GROUP_NOTICE:100,PROFILE_GET:101,PROFILE_SET:102,PROFILE_DATA:103};
let sock,buf=Buffer.alloc(0),inbox=[],waiters=[];
function attach(s){s.on('data',c=>{buf=Buffer.concat([buf,c]);while(buf.length>=MSG_SIZE){const m=decodeMsg(buf.slice(0,MSG_SIZE));buf=buf.slice(MSG_SIZE);onMsg(m);}});}
function onMsg(m){const i=waiters.findIndex(w=>w.pred(m));if(i>=0){const w=waiters[i];waiters.splice(i,1);clearTimeout(w.to);w.res(m);}else inbox.push(m);}
function take(pred,desc){const i=inbox.findIndex(pred);if(i>=0){const m=inbox[i];inbox.splice(i,1);return Promise.resolve(m);}
  return new Promise((res,rej)=>{const w={pred,res};w.to=setTimeout(()=>rej(new Error('timeout: '+desc)),4000);waiters.push(w);});}
const send=o=>sock.write(encodeMsg(o));
const byType=t=>(m=>m.type===t);
let pass=true; const chk=(n,ok,ex)=>{console.log((ok?'  ✅ ':'  ❌ ')+n+(ex?'  '+ex:''));if(!ok)pass=false;};
const NICK='功能测试_'+Math.floor(Math.random()*9000+1000);
function connectRetry(tries){return new Promise((res,rej)=>{
  const s=net.connect(8888,'127.0.0.1');
  s.once('connect',()=>res(s));
  s.once('error',()=>{ if(tries<=0)return rej(new Error('cannot connect'));
    setTimeout(()=>connectRetry(tries-1).then(res,rej),150);});});}
(async()=>{
  sock=await connectRetry(25); attach(sock);
  send({type:T.REGISTER,body:NICK+'\npw123456'});
  const reg=await take(byType(T.RESPONSE),'register'); const acc=reg.body.trim();
  console.log('注册账号:',acc);
  send({type:T.LOGIN,body:acc+'\npw123456'});
  await take(m=>m.type===T.RESPONSE&&m.body.includes('\n'),'login');
  console.log('已登录\n');

  console.log('[1] 个人资料 PROFILE_SET / GET');
  send({type:T.PROFILE_SET,body:'泰格猫\n1998-08-08'});
  await take(byType(T.RESPONSE),'profile set');
  send({type:T.PROFILE_GET,to_name:''});
  const pd=await take(byType(T.PROFILE_DATA),'profile data'); const pf=pd.body.split('\t');
  chk('昵称已更新为 泰格猫', pf[0]==='泰格猫', JSON.stringify(pd.body));
  chk('生日已保存 1998-08-08', pf[1]==='1998-08-08');

  console.log('\n[2] 好友备注 FRIEND_REMARK');
  send({type:T.FRIEND_REMARK,to_name:'100001',body:'我的小助理'});
  await take(byType(T.RESPONSE),'remark');
  inbox=inbox.filter(x=>x.type!==T.FRIEND_LIST);  // 丢弃登录时推送的旧列表
  send({type:T.FRIEND_LIST});
  const fl=await take(byType(T.FRIEND_LIST),'friend list');
  const line=fl.body.split('\n').find(l=>l.startsWith('100001'));
  chk('好友列表含备注列', line.split('\t').length>=6, JSON.stringify(line));
  chk('100001 备注=我的小助理', (line.split('\t')[5]||'')==='我的小助理');

  console.log('\n[3] 创建群 + 邀请入群 GROUP_INVITE + GROUP_MEMBERS');
  send({type:T.GROUP_CREATE,body:'测试派对群_'+Math.floor(Math.random()*9000)});
  const gc=await take(byType(T.RESPONSE),'group create'); const gid=parseInt(gc.body,10);
  chk('建群成功', gid>0, 'gid='+gid);
  send({type:T.GROUP_INVITE,group_id:gid,body:'100001\n100002'});
  const inv=await take(byType(T.RESPONSE),'invite');
  chk('邀请应答', /已邀请\s*2/.test(inv.body), JSON.stringify(inv.body));
  send({type:T.GROUP_MEMBERS,group_id:gid});
  const gm=await take(byType(T.GROUP_MEMBERS),'members');
  const accts=gm.body.split('\n').filter(Boolean).map(l=>l.split('\t')[0]);
  chk('GROUP_MEMBERS 有应答', accts.length>0, JSON.stringify(accts));
  chk('群成员含 100001', accts.includes('100001'));
  chk('群成员含 100002', accts.includes('100002'));

  console.log('\n[4] 群公告 GROUP_NOTICE');
  send({type:T.GROUP_NOTICE,group_id:gid,body:'每晚8点准时发车 🐾'});
  const n1=await take(byType(T.GROUP_NOTICE),'notice set');
  chk('设置后返回公告', n1.body==='每晚8点准时发车 🐾', JSON.stringify(n1.body));
  send({type:T.GROUP_NOTICE,group_id:gid,body:''});
  const n2=await take(byType(T.GROUP_NOTICE),'notice get');
  chk('查询返回持久化公告', n2.body==='每晚8点准时发车 🐾');

  console.log('\n===== '+(pass?'全部通过 ✅':'存在失败 ❌')+' =====');
  process.exit(pass?0:1);
})().catch(e=>{console.log('ERROR:',e.message);process.exit(2);});
