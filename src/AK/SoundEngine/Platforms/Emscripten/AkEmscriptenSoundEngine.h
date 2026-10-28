#pragma once
#include <AK/SoundEngine/Common/AkTypes.h>
#include <AK/Tools/Common/AkPlatformFuncs.h>
// Port-owned platform contract for a source-built browser Wwise backend.
// This does not describe the ABI of a vendor Emscripten SDK binary.
// Defaults and engine initialization must be implemented by that backend.
struct AkPlatformInitSettings {
    AkThreadProperties threadLEngine;
    AkThreadProperties threadOutputMgr;
    AkThreadProperties threadBankManager;
    AkThreadProperties threadMonitor;
    AkUInt32 uSampleRate;
    AkUInt16 uNumRefillsInVoice;
};
