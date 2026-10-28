#pragma once
#include "D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/dev/internal/RecordingToolCore/include/recCore_iRecordingToolCore.h"
namespace srt { namespace web {
// The real core is statically linked into the same Wasm module. DLL code
// replacement is unavailable; original create/release/memento functions remain.
class RecordingToolCoreLoaderImpl {
    bool acquired=false;
public:
    Bool TryToLoadDll() {if(acquired) return false;acquired=true;return true;}
    void UnloadDll() {acquired=false;}
    Bool IsDllLoaded() const {return acquired;}
    recCore::IRecordingToolCore* Call_CreateRecordingToolCore(recCore::TAllocateFunction allocate,recCore::TFreeFunction free) {return acquired ? ::CreateRecordingToolCore(allocate,free) : nullptr;}
    void Call_ReleaseRecordingToolCore(recCore::IRecordingToolCore* core) {RED_FATAL_ASSERT(acquired,"Recording core is not acquired");::ReleaseRecordingToolCore(core);}
    void* Call_ReleaseWithMemento(recCore::IRecordingToolCore* core) {RED_FATAL_ASSERT(acquired,"Recording core is not acquired");return ::ReleaseWithMemento(core);}
    Bool Call_RecreateRecordingToolCore(void* memento) {return acquired && ::RecreateRecordingToolCore(memento);}
};
}}
