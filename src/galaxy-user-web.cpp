#include "galaxy-state-web.h"
#include "D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/dev/external/gog/Include/galaxy/IUser.h"
#include <atomic>
#include <cstring>
namespace {
using namespace galaxy::api;
void Unavailable() {cpweb::SetGalaxyError(IError::INVALID_STATE,"Galaxy platform service is unavailable in this browser runtime");}
void Unauthorized() {cpweb::SetGalaxyError(IError::UNAUTHORIZED_ACCESS,"No authenticated Galaxy account or session");}
void ClearOutput(char* buffer,uint32_t length) {
    if(buffer&&length)buffer[0]=0;
    if(!buffer||!length)cpweb::SetGalaxyError(IError::INVALID_ARGUMENT,"Missing Galaxy output buffer");
    else Unauthorized();
}
class BrowserUser final:public IUser {
    std::atomic<uint64_t> generation{0};
    void Authenticate(IAuthListener* listener) {
        // No Windows Galaxy launcher or authenticated backend was initialized.
        // Preserve the native failed-authentication path; do not invent an ID,
        // token, license, authenticated anonymous session, or success callback.
        Unavailable();const auto request=generation.load();
        cpweb::QueueGalaxyCallback([this,listener,request] {
            if(request!=generation.load())return;
            auto registered=cpweb::GalaxyListeners(AUTH);bool delivered=false;
            for(auto* base:registered) {
                auto* target=static_cast<IAuthListener*>(base);
                target->OnAuthFailure(IAuthListener::FAILURE_REASON_GALAXY_SERVICE_NOT_AVAILABLE);
                if(target==listener)delivered=true;
            }
            if(listener&&!delivered)listener->OnAuthFailure(IAuthListener::FAILURE_REASON_GALAXY_SERVICE_NOT_AVAILABLE);
        });
    }
public:
    bool SignedIn() override {cpweb::ClearGalaxyError();return false;}
    bool IsLoggedOn() override {cpweb::ClearGalaxyError();return false;}
    GalaxyID GetGalaxyID() override {Unauthorized();return {};}
    void SignInCredentials(const char* login,const char* password,IAuthListener* listener) override {
        if(!login||!*login||!password||!*password) {cpweb::SetGalaxyError(IError::INVALID_ARGUMENT,"Missing sign-in credentials");return;}
        Authenticate(listener);
    }
    void SignInToken(const char* token,IAuthListener* listener) override {
        if(!token||!*token) {cpweb::SetGalaxyError(IError::INVALID_ARGUMENT,"Missing refresh token");return;}Authenticate(listener);
    }
    void SignInLauncher(IAuthListener* listener) override {Authenticate(listener);}
    void SignInSteam(const void* ticket,uint32_t size,const char* name,IAuthListener* listener) override {
        if(!ticket||!size||!name) {cpweb::SetGalaxyError(IError::INVALID_ARGUMENT,"Missing Steam sign-in parameters");return;}Authenticate(listener);
    }
    void SignInEpic(const char* token,const char* name,IAuthListener* listener) override {
        if(!token||!*token||!name) {cpweb::SetGalaxyError(IError::INVALID_ARGUMENT,"Missing Epic sign-in parameters");return;}Authenticate(listener);
    }
    void SignInGalaxy(bool,IAuthListener* listener) override {Authenticate(listener);}
    void SignInUWP(IAuthListener* listener) override {Authenticate(listener);}
    void SignInPS4(const char* id,IAuthListener* listener) override {
        if(!id||!*id) {cpweb::SetGalaxyError(IError::INVALID_ARGUMENT,"Missing console client ID");return;}Authenticate(listener);
    }
    void SignInXB1(const char* id,IAuthListener* listener) override {
        if(!id||!*id) {cpweb::SetGalaxyError(IError::INVALID_ARGUMENT,"Missing console user ID");return;}Authenticate(listener);
    }
    void SignInXBLive(const char* token,const char* signature,const char* market,const char* locale,IAuthListener* listener) override {
        if(!token||!signature||!market||!locale) {cpweb::SetGalaxyError(IError::INVALID_ARGUMENT,"Missing external sign-in parameters");return;}Authenticate(listener);
    }
    void SignInAnonymous(IAuthListener* listener) override {Authenticate(listener);}
    void SignInAnonymousTelemetry(IAuthListener* listener) override {Authenticate(listener);}
    void SignInServerKey(const char* key,IAuthListener* listener) override {
        if(!key||!*key) {cpweb::SetGalaxyError(IError::INVALID_ARGUMENT,"Missing server key");return;}Authenticate(listener);
    }
    void SignOut() override {++generation;cpweb::ClearGalaxyError();}
    void RequestUserData(GalaxyID,ISpecificUserDataListener*) override {Unauthorized();}
    bool IsUserDataAvailable(GalaxyID) override {Unauthorized();return false;}
    const char* GetUserData(const char*,GalaxyID) override {Unauthorized();return nullptr;}
    void GetUserDataCopy(const char*,char* buffer,uint32_t length,GalaxyID) override {ClearOutput(buffer,length);}
    void SetUserData(const char*,const char*,ISpecificUserDataListener*) override {Unauthorized();}
    uint32_t GetUserDataCount(GalaxyID) override {Unauthorized();return 0;}
    bool GetUserDataByIndex(uint32_t,char* key,uint32_t keyLength,char* value,uint32_t valueLength,GalaxyID) override {
        ClearOutput(key,keyLength);ClearOutput(value,valueLength);return false;
    }
    void DeleteUserData(const char*,ISpecificUserDataListener*) override {Unauthorized();}
    void RequestEncryptedAppTicket(const void*,uint32_t,IEncryptedAppTicketListener* listener) override {
        Unauthorized();cpweb::QueueGalaxyCallback([listener] {
            bool delivered=false;
            for(auto* base:cpweb::GalaxyListeners(IEncryptedAppTicketListener::GetListenerType())) {
                auto* target=static_cast<IEncryptedAppTicketListener*>(base);
                target->OnEncryptedAppTicketRetrieveFailure(IEncryptedAppTicketListener::FAILURE_REASON_CONNECTION_FAILURE);
                if(target==listener)delivered=true;
            }
            if(listener&&!delivered)listener->OnEncryptedAppTicketRetrieveFailure(IEncryptedAppTicketListener::FAILURE_REASON_CONNECTION_FAILURE);
        });
    }
    void GetEncryptedAppTicket(void*,uint32_t,uint32_t& size) override {size=0;Unauthorized();}
    SessionID GetSessionID() override {Unauthorized();return 0;}
    const char* GetAccessToken() override {Unauthorized();return nullptr;}
    void GetAccessTokenCopy(char* buffer,uint32_t length) override {ClearOutput(buffer,length);}
    bool ReportInvalidAccessToken(const char*,const char*) override {Unauthorized();return false;}
};
}
namespace galaxy {namespace api {
IUser* GALAXY_CALLTYPE User() {cpweb::ClearGalaxyError();static BrowserUser user;return &user;}
}}
