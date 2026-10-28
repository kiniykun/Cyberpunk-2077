#include "galaxy-state-web.h"
#include "D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/dev/external/gog/Include/galaxy/IListenerRegistrar.h"
#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <vector>
#include <deque>
namespace {
class Error final:public galaxy::api::IError {
public:
    Type type=INVALID_STATE;
    std::string message;
    const char* GetName() const override {
        switch(type) {
            case UNAUTHORIZED_ACCESS:return "UnauthorizedAccess";
            case INVALID_ARGUMENT:return "InvalidArgument";
            case INVALID_STATE:return "InvalidState";
            case RUNTIME_ERROR:return "RuntimeError";
        }
        return "RuntimeError";
    }
    const char* GetMsg() const override {return message.c_str();}
    Type GetType() const override {return type;}
};
thread_local Error lastError;
thread_local bool hasError=false;
class Registrar final:public galaxy::api::IListenerRegistrar {
    std::mutex mutex;
    std::array<std::vector<galaxy::api::IGalaxyListener*>,galaxy::api::LISTENER_TYPE_END> listeners;
public:
    std::vector<galaxy::api::IGalaxyListener*> Snapshot(galaxy::api::ListenerType type) {
        if(type<galaxy::api::LISTENER_TYPE_BEGIN||type>=galaxy::api::LISTENER_TYPE_END)return {};
        std::lock_guard<std::mutex> guard(mutex);return listeners[type];
    }
    void Register(galaxy::api::ListenerType type,galaxy::api::IGalaxyListener* listener) override {
        cpweb::ClearGalaxyError();
        if(type<galaxy::api::LISTENER_TYPE_BEGIN||type>=galaxy::api::LISTENER_TYPE_END||!listener) {
            cpweb::SetGalaxyError(galaxy::api::IError::INVALID_ARGUMENT,"Invalid Galaxy listener type or pointer");return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        auto& registered=listeners[type];
        if(std::find(registered.begin(),registered.end(),listener)==registered.end())registered.push_back(listener);
    }
    void Unregister(galaxy::api::ListenerType type,galaxy::api::IGalaxyListener* listener) override {
        cpweb::ClearGalaxyError();
        if(type<galaxy::api::LISTENER_TYPE_BEGIN||type>=galaxy::api::LISTENER_TYPE_END||!listener) {
            cpweb::SetGalaxyError(galaxy::api::IError::INVALID_ARGUMENT,"Invalid Galaxy listener type or pointer");return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        auto& registered=listeners[type];registered.erase(std::remove(registered.begin(),registered.end(),listener),registered.end());
    }
};
Registrar& GlobalRegistrar() {static Registrar registrar;return registrar;}
std::mutex callbackMutex;
std::deque<std::function<void()>> callbacks;
}
namespace cpweb {
void SetGalaxyError(galaxy::api::IError::Type type,const char* message) {lastError.type=type;lastError.message=message?message:"";hasError=true;}
void ClearGalaxyError() {hasError=false;lastError.message.clear();}
std::vector<galaxy::api::IGalaxyListener*> GalaxyListeners(galaxy::api::ListenerType type) {return GlobalRegistrar().Snapshot(type);}
bool QueueGalaxyCallback(std::function<void()> callback) {
    std::lock_guard<std::mutex> guard(callbackMutex);
    if(callbacks.size()>=4096) {SetGalaxyError(galaxy::api::IError::RUNTIME_ERROR,"Galaxy callback queue is full");return false;}
    callbacks.push_back(std::move(callback));return true;
}
void ProcessGalaxyCallbacks() {
    std::deque<std::function<void()>> pending;
    {std::lock_guard<std::mutex> guard(callbackMutex);pending.swap(callbacks);}
    for(auto& callback:pending)callback();
}
void DiscardGalaxyCallbacks() {std::lock_guard<std::mutex> guard(callbackMutex);callbacks.clear();}
}
namespace galaxy {namespace api {
const IError* GALAXY_CALLTYPE GetError() {return hasError?&lastError:nullptr;}
IListenerRegistrar* GALAXY_CALLTYPE ListenerRegistrar() {cpweb::ClearGalaxyError();return &GlobalRegistrar();}
}}
