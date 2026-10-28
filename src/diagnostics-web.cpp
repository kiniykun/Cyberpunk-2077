// Browser diagnostics. No native debugger attachment or OS crash reporter.
#include "build.h"
#include "dbgUtils.h"
#include "errorHandler.h"
#include "error-hooks-web.h"
#include <emscripten.h>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <pthread.h>

namespace dbgutils {
Bool GEnableTrace = false;
FILE* GTraceFile = nullptr;
Bool IsDebuggerAttached(Bool* valid, EConnection) {
    // Browser DevTools attachment cannot be queried through this native API.
    if (valid) *valid = false;
    return false;
}
Bool IsTraceEnabled() { return GEnableTrace; }
void VTrace(const char* format, va_list args) {
    char buffer[2048]{};
    red::VSNPrintF(buffer, sizeof(buffer), format, args);
    FILE* destination = GTraceFile ? GTraceFile : stderr;
    std::fputs(buffer, destination);
    std::fputc('\n', destination);
    std::fflush(destination);
}
void Trace(const char* format, ...) {
    va_list args;
    va_start(args, format);
    VTrace(format, args);
    va_end(args);
}
}

namespace red { namespace err {
namespace {
pthread_mutex_t registrationLock = PTHREAD_MUTEX_INITIALIZER;
ErrorHandlerHooks::ScriptCallstackVisitorFunc* scriptVisitor = nullptr;
bool registered = false;
void AssertHandler(const ErrorMessage& error) {
    std::fprintf(stderr, "CP2077 assertion: %s:%u: %s: %s\n",
        error.m_file ? error.m_file : "<unknown>", error.m_line,
        error.m_expression ? error.m_expression : "<unknown>",
        error.m_message ? error.m_message : "<no message>");
    emscripten_log(EM_LOG_ERROR | EM_LOG_C_STACK | EM_LOG_JS_STACK,
                   "Original engine assertion");
    std::fflush(stderr);
}
void RegisterAttachment(const char* path) {
    if (!path) return;
    // Record real browser filesystem paths for retrieval by the launcher.
    // This does not upload files or claim an OS minidump was produced.
    pthread_mutex_lock(&registrationLock);
    FILE* index = std::fopen("/tmp/cp2077-error-attachments.txt", "a");
    if (index) { std::fprintf(index, "%s\n", path); std::fclose(index); }
    else std::fprintf(stderr, "Cannot record error attachment: %s\n", path);
    pthread_mutex_unlock(&registrationLock);
}
void FailFast() { std::fflush(stderr); std::abort(); }
}
ErrorHandlerHooks RegisterErrorHooksOnceWeb(Uint32, const char* version,
    ErrorHandlerHooks::ScriptCallstackVisitorFunc* visitor) {
    pthread_mutex_lock(&registrationLock);
    if (!registered) {
        registered = true;
        std::fprintf(stderr, "CP2077 browser error handler: %s\n", version ? version : "<unknown>");
    }
    if (!scriptVisitor && visitor) scriptVisitor = visitor;
    ErrorHandlerHooks hooks;
    hooks.fnAssertHandler = AssertHandler;
    hooks.fnRegisterAttachment = RegisterAttachment;
    hooks.fnFailFastAbortProcess = FailFast;
    hooks.fnScriptCallstackVisitor = scriptVisitor;
    pthread_mutex_unlock(&registrationLock);
    return hooks;
}
}}
