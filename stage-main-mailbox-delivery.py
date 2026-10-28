"""Use the SDK postMessage fallback for browser-main wakeups only.

Worker futexes, task queues, callback completion and GPU ownership stay intact.
The original waitAsync path remains selectable with mailboxAtomics=1.
"""
from pathlib import Path
import hashlib,json
r=Path(__file__).resolve().parent;p=r/'build/game/cp2077.js';text=p.read_text()
marker='function __emscripten_thread_mailbox_await(pthread_ptr) {\n  pthread_ptr = bigintToI53Checked(pthread_ptr);'
addition='''
  if (!ENVIRONMENT_IS_PTHREAD) {
    if (!Module["cp2077MainMailboxDelivery"]) {
      Module["cp2077MainMailboxDelivery"] = new URLSearchParams(location.search).get("mailboxPostMessage") === "1" ? "postMessage" : "waitAsync";
    }
    // waiting_async starts at zero in the SDK mailbox initializer. Leaving
    // it zero selects the SDK's existing native postMessage notification.
    // Select once before any browser-main waitAsync waiter is registered.
    if (Module["cp2077MainMailboxDelivery"] === "postMessage") return;
  }
'''
if 'cp2077MainMailboxDelivery' not in text:
    assert text.count(marker)==1
    text=text.replace(marker,marker+addition,1)
text=text.replace('new URLSearchParams(location.search).get("mailboxAtomics") === "1" ? "waitAsync" : "postMessage"','new URLSearchParams(location.search).get("mailboxPostMessage") === "1" ? "postMessage" : "waitAsync"')
p.write_text(text)
(r/'build/main-mailbox-delivery-stage.json').write_text(json.dumps({'JS_SHA256':hashlib.sha256(p.read_bytes()).hexdigest(),'BrowserMainDelivery':'original waitAsync default; optional SDK postMessage fallback','WorkerWaitAsyncUnchanged':True,'OriginalTaskQueueUnchanged':True,'ComparisonQuery':'mailboxPostMessage=1','EngineFPSImprovementVerified':False},indent=2))
print('Original mailbox default preserved; optional SDK fallback available.')
