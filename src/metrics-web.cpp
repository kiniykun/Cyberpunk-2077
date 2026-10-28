#include "build.h"
#include "metrics-web.h"
namespace red { namespace memory {
void MetricsCaptureWeb::WritePlatfromIdentifier(Serializer& out){
    const char name[]="WASM64";const u16 bytes=sizeof(name)-1;
    out.Serialize(bytes);out.Serialize(name,bytes);
}
void MetricsCaptureWeb::WriteLoadedModules(Serializer& out){
    // Single statically linked Wasm module. Offsets are module-relative.
    // Code-section length is unknown here; zero denotes unavailable, not an
    // invented native address range. The profiling reader needs WASM64 support.
    const char prefix[]="MOD_",name[]="cp2077.wasm";
    const u64 size=0,base=0;const u32 bytes=sizeof(name)-1;
    out.Serialize(prefix,sizeof(prefix)-1);out.Serialize(size);
    out.Serialize(bytes);out.Serialize(name,bytes);out.Serialize(base);
}
}}
