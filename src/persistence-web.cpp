// Authored browser storage bridge. Keeps the browser event loop nonblocking.
#include "persistence-web.h"
#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <cstdio>

namespace {
struct FlushRequest { bool succeeded = false; };
em_proxying_queue* StorageQueue() {
    static em_proxying_queue* queue = em_proxying_queue_create();
    return queue;
}
}

extern "C" EMSCRIPTEN_KEEPALIVE void cp2077StorageFlushFinished(
    em_proxying_ctx* context, FlushRequest* request, int succeeded) {
    request->succeeded = succeeded != 0;
    emscripten_proxy_finish(context);
}

EM_JS(void, cp2077StorageFlushBegin, (em_proxying_ctx* context, FlushRequest* request), {
    let completed = false;
    const finish = function (succeeded) {
        if (completed) return;
        completed = true;
        _cp2077StorageFlushFinished(context, request, succeeded ? 1 : 0);
    };
    if (!Module['cp2077PersistentStorageReady'] ||
        typeof Module['cp2077FlushPersistentStorage'] !== 'function') {
        console.error('CP2077: persistent user storage is not initialized');
        finish(false);
        return;
    }
    try {
        Promise.resolve(Module['cp2077FlushPersistentStorage']()).then(
            function () { finish(true); },
            function (error) {
                console.error('CP2077: could not commit user storage', error);
                finish(false);
            });
    } catch (error) {
        console.error('CP2077: could not start user storage commit', error);
        finish(false);
    }
});

namespace cp2077::web {
bool FlushPersistentStorage() {
    if (emscripten_is_main_runtime_thread()) {
        std::fputs("CP2077: blocking storage commit requires an engine worker\n", stderr);
        return false;
    }
    auto* queue = StorageQueue();
    if (!queue) return false;
    FlushRequest request;
    // The context remains pending until the asynchronous IndexedDB callback
    // explicitly finishes it. The original service worker stays alive and waits;
    // the browser main thread returns immediately to process that callback.
    const bool completed = emscripten_proxy_sync_with_ctx(
        queue, emscripten_main_runtime_thread_id(),
        [](em_proxying_ctx* context, void* argument) {
            cp2077StorageFlushBegin(context, static_cast<FlushRequest*>(argument));
        }, &request);
    return completed && request.succeeded;
}
}
