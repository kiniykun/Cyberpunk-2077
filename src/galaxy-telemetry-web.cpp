#include "galaxy-state-web.h"
#include "D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/dev/external/gog/Include/galaxy/ITelemetry.h"
#include "D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/dev/external/gog/Include/galaxy/IUser.h"
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>
#include <unistd.h>
namespace {
using namespace galaxy::api;
class BrowserTelemetry final:public ITelemetry {
    enum class Kind {Object,Array,String,Integer,Float,Boolean};
    struct Param {
        Kind kind=Kind::Object;std::string name;
        std::variant<std::string,int32_t,double,bool> value;
        std::vector<std::unique_ptr<Param>> children;
    };
    std::mutex mutex;
    Param root;
    std::vector<Param*> stack{&root};
    size_t count=0,textBytes=0;
    uint32_t eventIndex=0;
    std::string sampling,visit;
    void Invalid(const char* message) {cpweb::SetGalaxyError(IError::INVALID_ARGUMENT,message);}
    Param* Add(const char* name,Kind kind) {
        if(count>=1024) {Invalid("Galaxy event parameter limit exceeded");return nullptr;}
        if(stack.back()->kind!=Kind::Array&&(!name||!*name)) {Invalid("Object parameter requires a name");return nullptr;}
        const size_t size=name?strnlen(name,4097):0;
        if(size>4096||textBytes>65536-size) {Invalid("Galaxy parameter text limit exceeded");return nullptr;}
        auto node=std::make_unique<Param>();node->kind=kind;if(name)node->name.assign(name,size);
        auto* result=node.get();stack.back()->children.push_back(std::move(node));++count;textBytes+=size;return result;
    }
    void Clear() {root.children.clear();stack.assign(1,&root);count=textBytes=0;}
    void Container(const char* name,Kind kind) {
        cpweb::ClearGalaxyError();std::lock_guard<std::mutex> guard(mutex);
        if(stack.size()>=16) {Invalid("Galaxy parameter nesting limit exceeded");return;}
        if(auto* node=Add(name,kind))stack.push_back(node);
    }
    void NewVisit() {
        unsigned char random[16];
        if(getentropy(random,sizeof(random))!=0) {cpweb::SetGalaxyError(IError::RUNTIME_ERROR,"Unable to generate Galaxy visit identifier");return;}
        static const char hex[]="0123456789abcdef";std::string next;next.reserve(32);
        for(auto byte:random) {next.push_back(hex[byte>>4]);next.push_back(hex[byte&15]);}
        visit=std::move(next);
    }
    uint32_t Send(const char* name,ITelemetryEventSendListener* listener,bool anonymous) {
        cpweb::ClearGalaxyError();std::string type;
        uint32_t index;bool valid;
        {
            std::lock_guard<std::mutex> guard(mutex);
            valid=name&&*name&&strnlen(name,4097)<=4096&&stack.size()==1;
            if(name&&strnlen(name,4097)<=4096)type=name;
            index=++eventIndex;
            // The SDK clears all accumulated parameters after every send call,
            // including requests that fail. No data is reported as transmitted.
            Clear();
        }
        if(!valid)Invalid("Invalid telemetry event name or unclosed parameter container");
        else cpweb::SetGalaxyError(anonymous?IError::INVALID_STATE:IError::UNAUTHORIZED_ACCESS,
            anonymous?"No Galaxy telemetry transport is configured":"No authenticated Galaxy telemetry session");
        const auto reason=valid?ITelemetryEventSendListener::FAILURE_REASON_CONNECTION_FAILURE:
            ITelemetryEventSendListener::FAILURE_REASON_INVALID_DATA;
        cpweb::QueueGalaxyCallback([type,index,listener,reason] {
            bool delivered=false;
            for(auto* base:cpweb::GalaxyListeners(TELEMETRY_EVENT_SEND_LISTENER)) {
                auto* target=static_cast<ITelemetryEventSendListener*>(base);
                target->OnTelemetryEventSendFailure(type.c_str(),index,reason);if(target==listener)delivered=true;
            }
            if(listener&&!delivered)listener->OnTelemetryEventSendFailure(type.c_str(),index,reason);
        });
        return index;
    }
public:
    void AddStringParam(const char* name,const char* value) override {
        cpweb::ClearGalaxyError();std::lock_guard<std::mutex> guard(mutex);
        const size_t size=value?strnlen(value,4097):4097;
        const size_t nameBytes=name?strnlen(name,4097):0;
        if(size>4096||nameBytes>4096||textBytes+nameBytes+size>65536) {Invalid("Invalid Galaxy string parameter");return;}
        if(auto* node=Add(name,Kind::String)) {node->value=std::string(value,size);textBytes+=size;}
    }
    void AddIntParam(const char* name,int32_t value) override {
        cpweb::ClearGalaxyError();std::lock_guard<std::mutex> guard(mutex);if(auto* node=Add(name,Kind::Integer))node->value=value;
    }
    void AddFloatParam(const char* name,double value) override {
        cpweb::ClearGalaxyError();std::lock_guard<std::mutex> guard(mutex);
        uint64_t bits;std::memcpy(&bits,&value,sizeof(bits));
        if((bits&0x7ff0000000000000ULL)==0x7ff0000000000000ULL) {Invalid("Non-finite Galaxy numeric parameter");return;}
        if(auto* node=Add(name,Kind::Float))node->value=value;
    }
    void AddBoolParam(const char* name,bool value) override {
        cpweb::ClearGalaxyError();std::lock_guard<std::mutex> guard(mutex);if(auto* node=Add(name,Kind::Boolean))node->value=value;
    }
    void AddObjectParam(const char* name) override {Container(name,Kind::Object);}
    void AddArrayParam(const char* name) override {Container(name,Kind::Array);}
    void CloseParam() override {
        cpweb::ClearGalaxyError();std::lock_guard<std::mutex> guard(mutex);
        if(stack.size()==1)Invalid("No Galaxy parameter container is open");else stack.pop_back();
    }
    void ClearParams() override {cpweb::ClearGalaxyError();std::lock_guard<std::mutex> guard(mutex);Clear();}
    void SetSamplingClass(const char* name) override {
        cpweb::ClearGalaxyError();std::lock_guard<std::mutex> guard(mutex);
        if(!name||strnlen(name,4097)>4096) {Invalid("Invalid Galaxy sampling class");return;}sampling=name;
    }
    uint32_t SendTelemetryEvent(const char* type,ITelemetryEventSendListener* listener) override {return Send(type,listener,false);}
    uint32_t SendAnonymousTelemetryEvent(const char* type,ITelemetryEventSendListener* listener) override {return Send(type,listener,true);}
    const char* GetVisitID() override {
        cpweb::ClearGalaxyError();std::lock_guard<std::mutex> guard(mutex);if(visit.empty())NewVisit();
        thread_local std::string snapshot;snapshot=visit;return snapshot.empty()?nullptr:snapshot.c_str();
    }
    void GetVisitIDCopy(char* buffer,uint32_t capacity) override {
        const char* id=GetVisitID();
        if(!buffer||!capacity) {Invalid("Missing visit ID output buffer");return;}
        buffer[0]=0;if(!id)return;
        const size_t length=std::strlen(id);
        if(length>=capacity) {Invalid("Visit ID output buffer is too small");return;}std::memcpy(buffer,id,length+1);
    }
    void ResetVisitID() override {cpweb::ClearGalaxyError();std::lock_guard<std::mutex> guard(mutex);NewVisit();}
};
}
namespace galaxy {namespace api {
ITelemetry* GALAXY_CALLTYPE Telemetry() {cpweb::ClearGalaxyError();static BrowserTelemetry telemetry;return &telemetry;}
}}
