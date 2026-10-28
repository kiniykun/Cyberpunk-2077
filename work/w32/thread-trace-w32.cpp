// [w32-threads] Names the creator of every pthread (first wasm frames of the JS stack) on stderr, so the thread
// count/owners show in the boot log and the page telemetry tail. Linked with -Wl,--wrap=pthread_create.
#include <emscripten/emscripten.h>
#include <pthread.h>
#include <atomic>
#include <cstdio>
#include <cstring>
extern "C" int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern "C" int __wrap_pthread_create(pthread_t* t, const pthread_attr_t* a, void* (*fn)(void*), void* arg) {
    static std::atomic<unsigned> count{0};
    const unsigned n = ++count;
    static thread_local char stack[8192]; char names[600] = {0}; unsigned used = 0, frames = 0;
    emscripten_get_callstack(EM_LOG_JS_STACK, stack, sizeof(stack));
    for (char* q = stack; frames < 6 && (q = std::strstr(q, "cp2077.wasm.")); ) {
        q += 12; char* e = std::strchr(q, '('); char* nl = std::strchr(q, '\n'); if (!e || (nl && nl < e)) e = nl ? nl : q + std::strlen(q);
        const unsigned len = unsigned(e - q);
        if (!len || !std::strncmp(q, "__wrap_pthread_create", 21) || !std::strncmp(q, "emscripten", 10)) continue;
        if (used + len + 3 >= sizeof(names)) break;
        if (used) { names[used++] = ' '; names[used++] = '<'; names[used++] = ' '; }
        std::memcpy(names + used, q, len); used += len; ++frames;
    }
    const int r = __real_pthread_create(t, a, fn, arg);
    std::fprintf(stderr, "CP2077 w32 thread #%u rc=%d :: %s\n", n, r, names);
    return r;
}
