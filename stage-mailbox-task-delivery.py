"""Keep self-mailbox notifications asynchronous without a timer task."""
from pathlib import Path
import hashlib,json
r=Path(__file__).resolve().parent
p=r/'build/game/cp2077.js'
text=p.read_text()
marker='var cp2077MailboxChannel = null;'
old='    setTimeout(checkMailbox);'
if marker not in text:
    anchor='function __emscripten_notify_mailbox_postmessage(targetThread, currThreadId) {'
    assert text.count(anchor)==1, 'SDK notification function changed'
    assert text.count(old)==1, 'SDK self-notification timer changed'
    helper='''var cp2077MailboxChannel = null;
var cp2077ScheduleMailboxTask = () => {
  if (!cp2077MailboxChannel) {
    cp2077MailboxChannel = new MessageChannel();
    cp2077MailboxChannel.port1.onmessage = () => {
      const stats = Module["cp2077MailboxTaskDiagnostics"];
      stats.delivered++;
      checkMailbox();
    };
    Module["cp2077MailboxTaskDiagnostics"] = {posted: 0, delivered: 0};
  }
  Module["cp2077MailboxTaskDiagnostics"].posted++;
  cp2077MailboxChannel.port2.postMessage(null);
};

'''
    text=text.replace(anchor,helper+anchor,1)
    text=text.replace(old,'    cp2077ScheduleMailboxTask();',1)
assert text.count(marker)==1
assert old not in text
assert text.count('    cp2077ScheduleMailboxTask();')==1
p.write_text(text)
(r/'build/mailbox-task-delivery-stage.json').write_text(json.dumps({
    'GeneratedJavaScriptSHA256':hashlib.sha256(p.read_bytes()).hexdigest(),
    'Scope':'Self-thread mailbox wakeups only. Cross-thread postMessage and Atomics.waitAsync unchanged.',
    'OnePostedTaskPerNotification':True,'RuntimeFreezeResolved':False
},indent=2))
print('Self-mailbox task delivery staged; runtime verification pending.')
