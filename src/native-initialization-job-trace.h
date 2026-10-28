#pragma once
#include <cstdio>
namespace cp2077 { namespace web {
// Observes the lifetime of the real callback. A returned callback does not
// imply that its asynchronous child jobs have finished or initialization won.
struct NativeInitializationJobTrace {
    const char* name;
    explicit NativeInitializationJobTrace(const char* value):name(value) {
        std::fprintf(stderr,"CP2077 native init job: phase=enter name=%s\n",name);
    }
    ~NativeInitializationJobTrace() {
        std::fprintf(stderr,"CP2077 native init job: phase=returned name=%s\n",name);
    }
};
} }
