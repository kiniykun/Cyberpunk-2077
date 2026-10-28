#include "galaxy-state-web.h"
#include "D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/dev/external/gog/Include/galaxy/INetworking.h"
#include "D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/dev/external/gog/Include/galaxy/IMatchmaking.h"
#include <cstring>
#include <mutex>
#include <string>
#include <variant>
#include <vector>
namespace {
using namespace galaxy::api;
void Unavailable() {cpweb::SetGalaxyError(IError::INVALID_STATE,"No initialized Galaxy peer transport or matchmaking service");}
void Invalid(const char* message) {cpweb::SetGalaxyError(IError::INVALID_ARGUMENT,message);}
void Clear(char* buffer,uint32_t capacity) {if(buffer&&capacity)buffer[0]=0;}
template<class Listener,class Function> void Notify(Listener* listener,Function function) {
    cpweb::QueueGalaxyCallback([listener,function] {
        bool delivered=false;
        for(auto* base:cpweb::GalaxyListeners(Listener::GetListenerType())) {
            auto* target=static_cast<Listener*>(base);function(target);if(target==listener)delivered=true;
        }
        if(listener&&!delivered)function(listener);
    });
}
class BrowserNetworking final:public INetworking {
public:
    bool SendP2PPacket(GalaxyID peer,const void* data,uint32_t size,P2PSendType type,uint8_t) override {
        if(!peer.IsValid()||!data||!size||type<P2P_SEND_UNRELIABLE||type>P2P_SEND_RELIABLE_IMMEDIATE)Invalid("Invalid Galaxy packet destination, bytes or send mode");
        else Unavailable();return false;
    }
    bool IsP2PPacketAvailable(uint32_t* size,uint8_t) override {
        if(size)*size=0;Unavailable();return false;
    }
    bool PeekP2PPacket(void*,uint32_t,uint32_t* size,GalaxyID& peer,uint8_t) override {
        if(size)*size=0;peer=GalaxyID();Unavailable();return false;
    }
    bool ReadP2PPacket(void* data,uint32_t capacity,uint32_t* size,GalaxyID& peer,uint8_t channel) override {
        return PeekP2PPacket(data,capacity,size,peer,channel);
    }
    void PopP2PPacket(uint8_t) override {Unavailable();}
    int GetPingWith(GalaxyID) override {Unavailable();return -1;}
    void RequestNatTypeDetection() override {
        Unavailable();Notify<INatTypeDetectionListener>(nullptr,[](auto* listener){listener->OnNatTypeDetectionFailure();});
    }
    NatType GetNatType() override {Unavailable();return NAT_TYPE_UNKNOWN;}
    ConnectionType GetConnectionType(GalaxyID) override {Unavailable();return CONNECTION_TYPE_NONE;}
};
class BrowserMatchmaking final:public IMatchmaking {
    struct Filter {std::string key;std::variant<std::string,int32_t> value;LobbyComparisonType comparison;bool near=false;};
    std::mutex mutex;std::vector<Filter> filters;uint32_t resultLimit=0;
    void FilterParam(const char* key,std::variant<std::string,int32_t> value,LobbyComparisonType comparison,bool near=false) {
        cpweb::ClearGalaxyError();
        if(!key||!*key||strnlen(key,4097)>4096||comparison<LOBBY_COMPARISON_TYPE_EQUAL||comparison>LOBBY_COMPARISON_TYPE_LOWER_OR_EQUAL) {
            Invalid("Invalid Galaxy lobby filter");return;
        }
        std::lock_guard<std::mutex> guard(mutex);
        if(filters.size()>=128) {Invalid("Galaxy lobby filter limit exceeded");return;}
        filters.push_back({key,std::move(value),comparison,near});
    }
    static void DataFailure(GalaxyID lobby,ILobbyDataUpdateListener* listener) {
        Unavailable();Notify(listener,[lobby](auto* target){target->OnLobbyDataUpdateFailure(lobby,ILobbyDataUpdateListener::FAILURE_REASON_CONNECTION_FAILURE);});
    }
    static void MemberFailure(GalaxyID lobby,ILobbyMemberDataUpdateListener* listener) {
        Unavailable();Notify(listener,[lobby](auto* target){target->OnLobbyMemberDataUpdateFailure(lobby,GalaxyID(),ILobbyMemberDataUpdateListener::FAILURE_REASON_CONNECTION_FAILURE);});
    }
public:
    void CreateLobby(LobbyType type,uint32_t members,bool,LobbyTopologyType topology,ILobbyCreatedListener* created,ILobbyEnteredListener*) override {
        if(type<LOBBY_TYPE_PRIVATE||type>LOBBY_TYPE_INVISIBLE_TO_FRIENDS||!members||members>250||topology<DEPRECATED_LOBBY_TOPOLOGY_TYPE_FCM_HOST_MIGRATION||topology>LOBBY_TOPOLOGY_TYPE_FCM_OWNERSHIP_TRANSITION) {
            Invalid("Invalid Galaxy lobby creation parameters");return;
        }
        Unavailable();Notify(created,[](auto* listener){listener->OnLobbyCreated(GalaxyID(),LOBBY_CREATE_RESULT_CONNECTION_FAILURE);});
    }
    void RequestLobbyList(bool,ILobbyListListener* listener) override {
        {std::lock_guard<std::mutex> guard(mutex);filters.clear();resultLimit=0;}
        Unavailable();Notify(listener,[](auto* target){target->OnLobbyList(0,LOBBY_LIST_RESULT_CONNECTION_FAILURE);});
    }
    void AddRequestLobbyListResultCountFilter(uint32_t limit) override {cpweb::ClearGalaxyError();std::lock_guard<std::mutex> guard(mutex);resultLimit=limit;}
    void AddRequestLobbyListStringFilter(const char* key,const char* value,LobbyComparisonType comparison) override {
        if(!value||strnlen(value,4097)>4096) {Invalid("Invalid Galaxy lobby string filter");return;}FilterParam(key,std::string(value),comparison);
    }
    void AddRequestLobbyListNumericalFilter(const char* key,int32_t value,LobbyComparisonType comparison) override {FilterParam(key,value,comparison);}
    void AddRequestLobbyListNearValueFilter(const char* key,int32_t value) override {FilterParam(key,value,LOBBY_COMPARISON_TYPE_EQUAL,true);}
    GalaxyID GetLobbyByIndex(uint32_t) override {Unavailable();return {};}
    void JoinLobby(GalaxyID lobby,ILobbyEnteredListener* listener) override {
        if(!lobby.IsValid()) {Invalid("Invalid Galaxy lobby ID");return;}
        Unavailable();Notify(listener,[lobby](auto* target){target->OnLobbyEntered(lobby,LOBBY_ENTER_RESULT_CONNECTION_FAILURE);});
    }
    void LeaveLobby(GalaxyID,ILobbyLeftListener*) override {Unavailable();}
    void SetMaxNumLobbyMembers(GalaxyID lobby,uint32_t members,ILobbyDataUpdateListener* listener) override {
        if(!members||members>250) {Invalid("Invalid lobby member limit");return;}DataFailure(lobby,listener);
    }
    uint32_t GetMaxNumLobbyMembers(GalaxyID) override {Unavailable();return 0;}
    uint32_t GetNumLobbyMembers(GalaxyID) override {Unavailable();return 0;}
    GalaxyID GetLobbyMemberByIndex(GalaxyID,uint32_t) override {Unavailable();return {};}
    void SetLobbyType(GalaxyID lobby,LobbyType,ILobbyDataUpdateListener* listener) override {DataFailure(lobby,listener);}
    LobbyType GetLobbyType(GalaxyID) override {Unavailable();return LOBBY_TYPE_PRIVATE;}
    void SetLobbyJoinable(GalaxyID lobby,bool,ILobbyDataUpdateListener* listener) override {DataFailure(lobby,listener);}
    bool IsLobbyJoinable(GalaxyID) override {Unavailable();return false;}
    void RequestLobbyData(GalaxyID lobby,ILobbyDataRetrieveListener* listener) override {
        Unavailable();Notify(listener,[lobby](auto* target){target->OnLobbyDataRetrieveFailure(lobby,ILobbyDataRetrieveListener::FAILURE_REASON_CONNECTION_FAILURE);});
    }
    const char* GetLobbyData(GalaxyID,const char*) override {Unavailable();return nullptr;}
    void GetLobbyDataCopy(GalaxyID,const char*,char* buffer,uint32_t capacity) override {Clear(buffer,capacity);Unavailable();}
    void SetLobbyData(GalaxyID lobby,const char*,const char*,ILobbyDataUpdateListener* listener) override {DataFailure(lobby,listener);}
    uint32_t GetLobbyDataCount(GalaxyID) override {Unavailable();return 0;}
    bool GetLobbyDataByIndex(GalaxyID,uint32_t,char* key,uint32_t keyLength,char* value,uint32_t valueLength) override {
        Clear(key,keyLength);Clear(value,valueLength);Unavailable();return false;
    }
    void DeleteLobbyData(GalaxyID lobby,const char*,ILobbyDataUpdateListener* listener) override {DataFailure(lobby,listener);}
    const char* GetLobbyMemberData(GalaxyID,GalaxyID,const char*) override {Unavailable();return nullptr;}
    void GetLobbyMemberDataCopy(GalaxyID,GalaxyID,const char*,char* buffer,uint32_t capacity) override {Clear(buffer,capacity);Unavailable();}
    void SetLobbyMemberData(GalaxyID lobby,const char*,const char*,ILobbyMemberDataUpdateListener* listener) override {MemberFailure(lobby,listener);}
    uint32_t GetLobbyMemberDataCount(GalaxyID,GalaxyID) override {Unavailable();return 0;}
    bool GetLobbyMemberDataByIndex(GalaxyID,GalaxyID,uint32_t,char* key,uint32_t keyLength,char* value,uint32_t valueLength) override {
        Clear(key,keyLength);Clear(value,valueLength);Unavailable();return false;
    }
    void DeleteLobbyMemberData(GalaxyID lobby,const char*,ILobbyMemberDataUpdateListener* listener) override {MemberFailure(lobby,listener);}
    GalaxyID GetLobbyOwner(GalaxyID) override {Unavailable();return {};}
    bool SendLobbyMessage(GalaxyID,const void*,uint32_t) override {Unavailable();return false;}
    uint32_t GetLobbyMessage(GalaxyID,uint32_t,GalaxyID& sender,char* message,uint32_t capacity) override {
        sender=GalaxyID();Clear(message,capacity);Unavailable();return 0;
    }
};
}
namespace galaxy {namespace api {
INetworking* GALAXY_CALLTYPE Networking() {cpweb::ClearGalaxyError();static BrowserNetworking networking;return &networking;}
IMatchmaking* GALAXY_CALLTYPE Matchmaking() {cpweb::ClearGalaxyError();static BrowserMatchmaking matchmaking;return &matchmaking;}
}}
