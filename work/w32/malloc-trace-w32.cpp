// [w32 diagnostic] Large-malloc attribution, linked only with CPWEB_MALLOC_TRACE=1 (link-w32.py adds
// -Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc,--wrap=memalign,--wrap=aligned_alloc,--wrap=posix_memalign).
// Allocations >= 256 KiB record the first non-allocator frames of their call stack; frees remove them. Report via
// CpWebMallocTraceReport() (called by the memory probe). emscripten_builtin_* (engine allocator ranges) are not wrapped.
#include <emscripten/emscripten.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
extern "C" {
void* __real_malloc(size_t); void __real_free(void*); void* __real_calloc(size_t, size_t); void* __real_realloc(void*, size_t);
void* __real_memalign(size_t, size_t); void* __real_aligned_alloc(size_t, size_t); int __real_posix_memalign(void**, size_t, size_t);
}
namespace {
constexpr size_t kMin = 256 * 1024;
constexpr unsigned kSlots = 1u << 16, kSites = 512;
struct Slot { uintptr_t ptr; size_t bytes; unsigned site; };
Slot slots[kSlots];
struct Site { char name[192]; uint64_t bytes; unsigned count; };
Site sites[kSites]; unsigned siteCount = 0;
std::atomic_flag lockFlag = ATOMIC_FLAG_INIT;
thread_local bool inTrace = false;
struct Lock { Lock() { while (lockFlag.test_and_set(std::memory_order_acquire)) {} } ~Lock() { lockFlag.clear(std::memory_order_release); } };
unsigned Hash(uintptr_t p) { return unsigned((p >> 4) * 2654435761u) & (kSlots - 1); }
unsigned SiteOf() {
    static thread_local char stack[16384]; char name[192] = {0}; unsigned used = 0, frames = 0;
    emscripten_get_callstack(EM_LOG_JS_STACK, stack, sizeof(stack));
    for (char* q = stack; frames < 3 && (q = std::strstr(q, "cp2077.wasm.")); ) {
        q += 12; char* e = std::strchr(q, '('); char* nl = std::strchr(q, '\n'); if (!e || (nl && nl < e)) e = nl ? nl : q + std::strlen(q);
        const unsigned len = unsigned(e - q);
        if (!std::strncmp(q, "__wrap_", 7) || !std::strncmp(q, "operator new", 12) || !std::strncmp(q, "std::", 5) || !std::strncmp(q, "dlmalloc", 8) ||
            !std::strncmp(q, "malloc", 6) || !std::strncmp(q, "calloc", 6) || !std::strncmp(q, "realloc", 7) || !std::strncmp(q, "memalign", 8) ||
            !std::strncmp(q, "aligned_alloc", 13) || !std::strncmp(q, "posix_memalign", 14) || !std::strncmp(q, "emscripten_builtin", 18)) continue;
        if (used + len + 2 >= sizeof(name)) break;
        if (used) name[used++] = '<'; std::memcpy(name + used, q, len); used += len; ++frames;
    }
    for (unsigned i = 0; i < siteCount; ++i) if (!std::strcmp(sites[i].name, name)) return i;
    if (siteCount == kSites) return kSites - 1;
    std::memcpy(sites[siteCount].name, name, sizeof(name)); return siteCount++;
}
void Track(void* p, size_t n) {
    if (!p || n < kMin || inTrace) return;
    inTrace = true; Lock lock; const unsigned s = SiteOf();
    for (unsigned h = Hash(uintptr_t(p)), i = 0; i < kSlots; ++i, h = (h + 1) & (kSlots - 1))
        if (!slots[h].ptr) { slots[h] = {uintptr_t(p), n, s}; sites[s].bytes += n; ++sites[s].count; break; }
    inTrace = false;
}
void Untrack(void* p) {
    if (!p) return;
    Lock lock;
    for (unsigned h = Hash(uintptr_t(p)), i = 0; i < kSlots && slots[h].ptr; ++i, h = (h + 1) & (kSlots - 1))
        if (slots[h].ptr == uintptr_t(p)) {
            sites[slots[h].site].bytes -= slots[h].bytes; --sites[slots[h].site].count;
            // backward-shift delete keeps probe chains intact
            unsigned j = h;
            for (;;) {
                slots[j] = {}; unsigned k = j;
                for (;;) { k = (k + 1) & (kSlots - 1); if (!slots[k].ptr) return; const unsigned home = Hash(slots[k].ptr);
                    if ((j <= k) ? (home <= j || home > k) : (home <= j && home > k)) break; }
                slots[j] = slots[k]; j = k;
            }
        }
}
}
extern "C" {
void* __wrap_malloc(size_t n) { void* p = __real_malloc(n); Track(p, n); return p; }
void __wrap_free(void* p) { if (p) Untrack(p); __real_free(p); }
void* __wrap_calloc(size_t a, size_t b) { void* p = __real_calloc(a, b); Track(p, a * b); return p; }
void* __wrap_realloc(void* old, size_t n) { if (old) Untrack(old); void* p = __real_realloc(old, n); Track(p ? p : nullptr, n); return p; }
void* __wrap_memalign(size_t a, size_t n) { void* p = __real_memalign(a, n); Track(p, n); return p; }
void* __wrap_aligned_alloc(size_t a, size_t n) { void* p = __real_aligned_alloc(a, n); Track(p, n); return p; }
int __wrap_posix_memalign(void** out, size_t a, size_t n) { const int r = __real_posix_memalign(out, a, n); if (!r) Track(*out, n); return r; }
EMSCRIPTEN_KEEPALIVE void CpWebMallocTraceReport() {
    Site copy[kSites]; unsigned n;
    { Lock lock; n = siteCount; std::memcpy(copy, sites, sizeof(Site) * n); }
    for (unsigned i = 0; i < n; ++i) for (unsigned j = i + 1; j < n; ++j) if (copy[j].bytes > copy[i].bytes) { Site t = copy[i]; copy[i] = copy[j]; copy[j] = t; }
    uint64_t total = 0; for (unsigned i = 0; i < n; ++i) total += copy[i].bytes;
    std::fprintf(stderr, "CP2077 w32 malloc trace: liveLargeMiB=%.1f sites=%u\n", total / 1048576.0, n);
    for (unsigned i = 0; i < n && i < 30; ++i) if (copy[i].bytes)
        std::fprintf(stderr, "CP2077 w32 malloc site: MiB=%.1f count=%u site=%s\n", copy[i].bytes / 1048576.0, copy[i].count, copy[i].name);
}
}
