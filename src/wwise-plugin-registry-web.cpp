// Browser plugin bundles are compiled into the actual Wwise module. Windows
// DLLs cannot be loaded here; unknown bundles return a real failure.
#include "AkEffectsMgr.h"
#include <AK/SoundEngine/Common/AkSoundEngine.h>
#include <AK/SoundEngine/Common/IAkPlugin.h>
#include <cstring>
extern AK::PluginRegistration CDPVoiceBroadcastSendFXRegistration;
extern AK::PluginRegistration CDPVoiceBroadcastSendStereoFXRegistration;
extern AK::PluginRegistration CDPVoiceBroadcastReceiveFXRegistration;
extern AK::PluginRegistration CDPVoiceBroadcastReceiveStereoFXRegistration;
namespace {
AKRESULT RegisterOriginal(AK::PluginRegistration& original) {
    if(!original.m_pCreateFunc||!original.m_pCreateParamFunc)return AK_InvalidFile;
    // Register exactly this real factory rather than its unrelated static-list
    // successors. The copy leaves the original startup registration list intact.
    AK::PluginRegistration selected=original;selected.pNext=nullptr;
    return CAkEffectsMgr::RegisterPluginList(&selected);
}
}
namespace AK { namespace SoundEngine {
AKRESULT RegisterPluginDLL(const AkOSChar* name,const AkOSChar* path) {
    if(!name||!*name)return AK_InvalidParameter;
    if(path&&*path)return AK_NotCompatible;
    const size_t length=std::strlen(name);
    if(length>=128||std::strchr(name,'/')||std::strchr(name,'\\'))return AK_FileNotFound;
    char bundle[128];std::memcpy(bundle,name,length+1);
    char* extension=std::strrchr(bundle,'.');
    if(extension) {
        if(std::strcmp(extension,".dll")&&std::strcmp(extension,".so")&&std::strcmp(extension,".wasm"))return AK_FileNotFound;
        *extension=0;
    }
    AK::PluginRegistration* registrations[4]={};unsigned count=0;
    if(!std::strcmp(bundle,"CDPVoiceBroadcast")||!std::strcmp(bundle,"CDPVoiceBroadcastSendFX")) {
        registrations[count++]=&CDPVoiceBroadcastSendFXRegistration;
        registrations[count++]=&CDPVoiceBroadcastSendStereoFXRegistration;
    }
    if(!std::strcmp(bundle,"CDPVoiceBroadcast")||!std::strcmp(bundle,"CDPVoiceBroadcastReceiveFX")) {
        registrations[count++]=&CDPVoiceBroadcastReceiveFXRegistration;
        registrations[count++]=&CDPVoiceBroadcastReceiveStereoFXRegistration;
    }
    if(!count)return AK_FileNotFound;
    for(unsigned i=0;i<count;++i) {const auto result=RegisterOriginal(*registrations[i]);if(result!=AK_Success)return result;}
    return AK_Success;
}
}}
