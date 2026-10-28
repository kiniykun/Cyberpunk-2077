const fs=require('node:fs');
const vm=require('node:vm');
const {MessageChannel}=require('node:worker_threads');
const source=fs.readFileSync('D:/2077/build/game/cp2077.js','utf8');
const start=source.indexOf('var cp2077MailboxChannel = null;');
const end=source.indexOf('function __emscripten_notify_mailbox_postmessage',start);
if(start<0||end<start)throw Error('Missing staged mailbox scheduler');
let delivered=0,finish;
const done=new Promise(resolve=>finish=resolve);
const context=vm.createContext({MessageChannel,Module:{},checkMailbox:()=>{
  delivered++;
  if(delivered===1)vm.runInContext('for(let i=0;i<10;i++)cp2077ScheduleMailboxTask()',context);
  if(delivered===210)finish();
}});
vm.runInContext(source.slice(start,end),context);
vm.runInContext('for(let i=0;i<200;i++)cp2077ScheduleMailboxTask()',context);
if(delivered!==0)throw Error('Mailbox processing ran synchronously');
const deadline=setTimeout(()=>{throw Error('Mailbox notification lost or stalled');},2000);
done.then(()=>{
  clearTimeout(deadline);
  const stats=context.Module.cp2077MailboxTaskDiagnostics;
  if(stats.posted!==210||stats.delivered!==210)throw Error('Duplicate or lost task');
  context.cp2077MailboxChannel.port1.close();context.cp2077MailboxChannel.port2.close();
  console.log(JSON.stringify({asynchronous:true,posted:stats.posted,delivered:stats.delivered,nestedNotifications:true,passed:true}));
});
