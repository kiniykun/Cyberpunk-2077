"""Stage the original service workflow with a real browser-local profile backend.

Original save/menu request machinery is retained. Store SDK calls have no
browser implementation and explicitly report NotSupported rather than success.
"""
import hashlib,json,re
from pathlib import Path
ROOT=Path(__file__).resolve().parent
SOURCE=Path(r'D:\cp2077SourceCode\extracted\root\R6.Root\Mainline\dev\src')
ORIGINAL=SOURCE/'win32/gameServicesWin'
original_cpp=ORIGINAL/'src/gameServicesWin.cpp'
original_header=ORIGINAL/'include/gameServicesWin.h'
cpp=original_cpp.read_text(encoding='utf-8-sig')
header=original_header.read_text(encoding='utf-8-sig')
notice=cpp[:cpp.index('#include')]
state=re.search(r'enum class GameServicesWin::State : Uint32\s*\{.*?\};',cpp,re.S)
if not state:raise RuntimeError('Original service states missing')
body=cpp[cpp.index('\tGameServicesWin::GameServicesWin()'):]
body=body.replace('GameServicesWinWorker','GameServicesWebWorker').replace('GameServicesWin','GameServicesWeb')
body=body.replace(': m_platformImpl( this )\n\t\t, m_state', ': m_state')
body=body.replace('\t\tUpdateGalaxy();\n','').replace('\t\tm_platformImpl.Update();\n','')
body=body.replace('\t\tShutdownGalaxy();\n','')
body=body.replace('\t\tGameServices::Update();','\t\tGameServices::Update();\n        cpweb::ProcessGalaxyCallbacks();')
body=body.replace('GameServices::Shutdown();','cpweb::DiscardGalaxyCallbacks();\n        GameServices::Shutdown();')
body=body.replace('\t\t\tm_platformImpl.Uninitialize();\n','').replace('\t\t\tm_platformImpl.UnloadLibrary();\n','')
body=body.replace('\t\tm_saveView.path = metadata->m_path;\n','')
body=body.replace('Unknown state %lu", state','Unknown state %u", static_cast<Uint32>(state)')

def replace_body(name,new_body):
    global body
    start=body.index('GameServicesWeb::'+name+'(')
    brace=body.index('{',start)
    # Original method strings/comments can contain braces; replace only using
    # a lexical scan with string/comment tokens removed at the same offsets.
    masked=re.sub(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/',lambda m:' '*len(m[0]),body,flags=re.S)
    depth=1;end=brace+1
    while depth:
        if masked[end]=='{':depth+=1
        elif masked[end]=='}':depth-=1
        end+=1
    body=body[:brace]+'{\n'+new_body+'\n\t}'+body[end:]

replace_body('Initialize','''        if (!GameServices::Initialize(appType)) return false;
        if (appType != red::AppType::Game) return true;
        Uint64 profileID = 0;
        if (!LoadBrowserProfile(profileID)) {
            RED_LOG_ERROR("Browser local profile initialization failed");
            GameServices::Shutdown();
            return false;
        }
        char language[32] = {};
        BrowserLanguage(language, sizeof(language));
        m_language = language;
        m_user = red::CreateUniquePtr<BrowserProfile>(profileID);
        m_worker = red::CreateUniquePtr<GameServicesWebWorker>(m_workerMutex, m_wakeUpWorker);
        m_worker->InitThread();
        m_worker->SetPriority(red::TP_Normal);
        AssignUser(0);
        return true;''')
replace_body('GetPlatform','        return red::Platform::Web;')
replace_body('GetPlatformName','        return { "Browser" };')
replace_body('GetLanguageCodeName','        return m_language;')
replace_body('Shutdown','''        if (m_worker) {
            m_worker->Exit();
            m_wakeUpWorker.WakeAll();
            m_worker->JoinThread();
            m_worker.Reset();
        }
        m_user.Reset();
        m_hasAssignedUser.SetValue(false);
        GameServices::Shutdown();''')
replace_body('AssignUser','''        if (!m_worker || !m_user) return;
        RequestSaves([this](const GameSaveViews& saves, const ServiceError errorCode) {
            if (errorCode != ServiceError_OK) {
                m_hasAssignedUser.SetValue(false);
                PushMessage(RED_NAME_CONSTEXPR("GameServicesAssignUser"), "failure");
                return;
            }
            if (saves.Empty()) { OnUserAssigned(); return; }
            LoadMetadata(saves[0], [this](const GameSaveView&, const save::MetadataPtr& metadata,
                                        const img::ImagePtr&, const ServiceError metadataError) {
                if (metadataError == ServiceError_OK) m_latestSaveMetadata = metadata;
                else RED_LOG_WARNING("Browser latest-save metadata could not be read");
                OnUserAssigned();
            });
        });''')
replace_body('OnAchievementUnlock','''        m_lastError = ServiceError_NotSupported;
        OnAchievementUnlocked();''')
replace_body('OnRichPresenceSet','''        m_lastError = ServiceError_NotSupported;
        OnRichPresenceSetFinished();''')
replace_body('OnOpenWebBrowser','''        if (!OpenBrowserURL(url.AsChar())) {
            RED_LOG_WARNING("Browser could not open external URL");
        }''')
# Original settings/profile serializers own and release their IFile argument
# before returning. Acknowledge only after browser storage has committed it.
for callback in ['GameServices::OnInGameConfigSaveFinished();','GameServices::OnUserFileSaveFinished();']:
    if body.count(callback)!=1:raise RuntimeError('Original file-save completion changed')
    body=body.replace(callback,'if (m_lastError == ServiceError_OK && !cp2077::web::FlushPersistentStorage())\n            m_lastError = ServiceError_InternalError;\n        '+callback)
if 'm_platformImpl' in body:raise RuntimeError('Native platform wrapper reference remains')

class_header=header[header.index('\tclass GAME_SERVICES_WIN_API GameServicesWin'):]
class_header=class_header.replace('GAME_SERVICES_WIN_API','GAME_SERVICES_API').replace('GameServicesWinWorker','GameServicesWebWorker').replace('GameServicesWin','GameServicesWeb')
class_header=class_header.replace('\t\tPlatformImpl m_platformImpl;', '\t\tred::UniquePtr<IGameServicesUser> m_user;\n\t\tred::String m_language;')
class_header=class_header.replace('\t\tvirtual Bool HasUserAssigned() const override;', '\t\tvirtual Bool HasUserAssigned() const override;\n\t\tvirtual IGameServicesUser* GetUser() const override { return m_user.Get(); }')
header=notice+'''#pragma once
#include "common/gameServices/include/gameServices.h"
#include "common/gameServices/include/gameServicesUser.h"
namespace services {
class GameServicesWebWorker;
'''+class_header

browser_code=r'''
EM_JS(void, BrowserLanguage, (char* output, size_t capacity), {
    const language = (navigator.language || 'en-US').toLowerCase();
    stringToUTF8(language, Number(output), Number(capacity));
});
EM_JS(int, BrowserOpenURL, (const char* address), {
    const url = UTF8ToString(Number(address));
    try {
        const target = new URL(url);
        if (target.protocol !== 'https:' && target.protocol !== 'http:') return 0;
        return window.open(target.href, '_blank', 'noopener') ? 1 : 0;
    } catch (error) { return 0; }
});
namespace {
bool OpenBrowserURL(const char* url) {
    struct Call { const char* url; bool opened = false; } call{url};
    static em_proxying_queue* queue = em_proxying_queue_create();
    if (!queue) return false;
    auto invoke = [](void* p) {
        auto* call = static_cast<Call*>(p);
        call->opened = BrowserOpenURL(call->url) != 0;
    };
    if (emscripten_is_main_runtime_thread()) invoke(&call);
    else if (!emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(), invoke, &call)) return false;
    return call.opened;
}
bool LoadBrowserProfile(Uint64& id) {
    constexpr char magic[8] = {'C','P','W','E','B','0','1','\0'};
    constexpr const char* path = "/userdata/browser-profile.bin";
    if (FILE* file = std::fopen(path, "rb")) {
        char savedMagic[8];
        bool valid = std::fread(savedMagic, 1, 8, file) == 8 &&
                     std::memcmp(savedMagic, magic, 8) == 0 &&
                     std::fread(&id, 1, sizeof(id), file) == sizeof(id) && id != 0 &&
                     std::fgetc(file) == EOF && !std::ferror(file);
        valid = std::fclose(file) == 0 && valid;
        return valid; // Do not replace an unreadable/corrupt existing profile.
    }
    if (errno != ENOENT) return false;
    do { if (getentropy(&id, sizeof(id)) != 0) return false; } while (!id);
    FILE* file = std::fopen(path, "wb");
    if (!file) return false;
    bool written = std::fwrite(magic, 1, 8, file) == 8 &&
                   std::fwrite(&id, 1, sizeof(id), file) == sizeof(id);
    written = std::fflush(file) == 0 && written;
    written = std::fclose(file) == 0 && written;
    return written && cp2077::web::FlushPersistentStorage();
}
}
namespace services {
class BrowserProfile final : public IGameServicesUser {
public:
    explicit BrowserProfile(Uint64 id) : m_id(id) {}
    UserId GetUserId() const override { return m_id; }
    String GetUserDisplayName() const override { return "Browser local profile"; }
    String GetUserRegion() const override { return ""; }
    Bool IsGuest() const override { return true; }
private:
    Uint64 m_id;
};
}
'''
cpp=notice+'''#include "common/core/include/corePublic.h"
#include "common/gameServices/include/gameServicesInternal.h"
#include "game-services-web.h"
#include "game-services-worker-web.h"
#include "persistence-web.h"
#include "galaxy-state-web.h"
#include "common/redFileSystem/include/fileSys.h"
#include "common/redFileSystem/include/nullFile.h"
#include "common/redConfig/include/inGameConfigSystem.h"
#include "common/gameSave/include/saveGame.h"
#include <emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/proxying.h>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <unistd.h>
RTTI_BEGIN_TYPE_IN_NAMESPACE(GameServicesWeb, services);
    RTTI_PARENT_TYPE(services::GameServices)
RTTI_END_TYPE();
'''+browser_code+'\nnamespace services {\n'+state[0].replace('GameServicesWin','GameServicesWeb')+'\n'+body
records=[]
for original,text,name in [(original_header,header,'game-services-web.h'),(original_cpp,cpp,'game-services-web.cpp')]:
    output=ROOT/'src'/name
    if not output.exists() or output.read_text()!=text:output.write_text(text,encoding='utf-8')
    records.append({'OriginalSource':str(original),'OriginalSHA256':hashlib.sha256(original.read_bytes()).hexdigest(),'AdaptedSource':str(output),'AdaptedSHA256':hashlib.sha256(output.read_bytes()).hexdigest()})
(ROOT/'build/game-services-web-source-manifest.json').write_text(json.dumps(records,indent=2))
print('Staged original service workflow with browser-local profile and storage backend')
