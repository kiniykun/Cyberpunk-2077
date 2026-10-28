// Authored browser transport for the original redHTTP AsyncClient API.
#include "common/redHTTP/src/build.h"
#include "http-web.h"
#include "common/redHTTP/include/httpAsyncClient.h"
#include <emscripten.h>
#include <emscripten/fetch.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace http {
namespace {
struct Transfer {
    std::mutex mutex;
    AsyncRequestPtr request;
    emscripten_fetch_t* fetch = nullptr;
    std::string url;
    std::vector<std::string> headers;
    std::vector<const char*> headerPointers;
    char method[5]{};
    unsigned timeout = 0;
    double started = 0;
    bool canceled = false;
    bool finished = false;
    bool delivered = false;
    AsyncRequestResult result = AsyncRequestResult::FailedToExecute;
    Uint32 status = 0;
    red::String body;
};
em_proxying_queue* MainQueue() {
    static em_proxying_queue* queue = em_proxying_queue_create();
    return queue;
}
void OnMain(void (*function)(void*), void* argument) {
    if (emscripten_is_main_runtime_thread()) function(argument);
    else if (!emscripten_proxy_sync(MainQueue(), emscripten_main_runtime_thread_id(), function, argument)) {
        std::fputs("CP2077: HTTP main-thread dispatch failed\n", stderr);
        std::abort();
    }
}
void Finished(emscripten_fetch_t* fetch) {
    auto* transfer = static_cast<Transfer*>(fetch->userData);
    std::lock_guard<std::mutex> guard(transfer->mutex);
    if (transfer->canceled) return;
    transfer->status = fetch->status;
    // A completed HTTP error response is still an HTTP response, as in CURL.
    transfer->result = fetch->status ? AsyncRequestResult::Ok :
        (transfer->timeout && emscripten_get_now()-transfer->started >= transfer->timeout ?
         AsyncRequestResult::Timeout : AsyncRequestResult::CouldntConnect);
    if (fetch->numBytes > std::numeric_limits<Uint32>::max()) {
        transfer->result = AsyncRequestResult::FailedToExecute;
    } else if (fetch->data && fetch->numBytes) {
        transfer->body = red::String(fetch->data, static_cast<Uint32>(fetch->numBytes));
    }
    transfer->finished = true;
}
void StartMain(void* argument) {
    auto* transfer = static_cast<Transfer*>(argument);
    emscripten_fetch_attr_t attributes;
    emscripten_fetch_attr_init(&attributes);
    std::strcpy(attributes.requestMethod, transfer->method);
    attributes.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    attributes.userData = transfer;
    attributes.onsuccess = Finished;
    attributes.onerror = Finished;
    attributes.timeoutMSecs = transfer->timeout;
    attributes.requestHeaders = transfer->headerPointers.data();
    transfer->started = emscripten_get_now();
    transfer->fetch = emscripten_fetch(&attributes, transfer->url.c_str());
}
void CloseMain(void* argument) {
    auto* transfer = static_cast<Transfer*>(argument);
    // close may call onerror synchronously. Never hold the callback's mutex here.
    if (transfer->fetch) {
        emscripten_fetch_close(transfer->fetch);
        transfer->fetch = nullptr;
    }
}
}
struct WebImpl::Storage {
    explicit Storage(AsyncClient& owner) : client(owner) {}
    AsyncClient& client;
    std::mutex mutex;
    std::vector<std::shared_ptr<Transfer>> transfers;
};
WebImpl::WebImpl(AsyncClient& client) : m_storage(new Storage(client)) {}
WebImpl::~WebImpl() {
    std::vector<std::shared_ptr<Transfer>> remaining;
    { std::lock_guard<std::mutex> guard(m_storage->mutex); remaining.swap(m_storage->transfers); }
    for (auto& transfer : remaining) {
        { std::lock_guard<std::mutex> guard(transfer->mutex); transfer->canceled = true; }
        OnMain(CloseMain, transfer.get());
    }
}
Bool WebImpl::StartExecuteRequest(AsyncRequest* request) {
    if (request->GetFlags() & AsyncRequestFlags_SkipVerifySSL) {
        std::fputs("CP2077: browser HTTP cannot disable TLS verification\n", stderr);
        return false;
    }
    auto transfer = std::make_shared<Transfer>();
    transfer->request = request->SharedFromThis();
    transfer->url = request->GetURI().AsChar();
    std::strcpy(transfer->method, request->GetMethod()==Method::GET ? "GET" : "POST");
    const uint64_t timeout = uint64_t(request->GetTimeout())*1000;
    if (timeout > std::numeric_limits<unsigned>::max()) return false;
    transfer->timeout = static_cast<unsigned>(timeout);
    for (const auto& header : request->GetHeaders()) {
        transfer->headers.emplace_back(header.name.AsChar());
        transfer->headers.emplace_back(header.value.AsChar());
    }
    for (const auto& header : transfer->headers) transfer->headerPointers.push_back(header.c_str());
    transfer->headerPointers.push_back(nullptr);
    OnMain(StartMain, transfer.get());
    if (!transfer->fetch) return false;
    request->SetRequestImplHandle(transfer.get());
    { std::lock_guard<std::mutex> guard(m_storage->mutex); m_storage->transfers.push_back(transfer); }
    return true;
}
void WebImpl::Update() {
    std::vector<std::shared_ptr<Transfer>> snapshot;
    { std::lock_guard<std::mutex> guard(m_storage->mutex); snapshot=m_storage->transfers; }
    for (const auto& transfer : snapshot) {
        AsyncRequestResult result;
        Uint32 status;
        red::String body;
        {
            std::lock_guard<std::mutex> guard(transfer->mutex);
            if (!transfer->finished || transfer->canceled || transfer->delivered) continue;
            transfer->delivered = true;
            result = transfer->result;
            status = transfer->status;
            body = std::move(transfer->body);
            // Publish under the transfer lock so AbortExecute cannot remove the
            // original executing request between this check and publication.
            // The original client still dispatches listeners from its Update.
            m_storage->client.ImplHandleRequestFinished(transfer->request.Get(), result, std::move(body), status);
        }
    }
}
void WebImpl::AbortExecute(void* handle) {
    auto* transfer = static_cast<Transfer*>(handle);
    { std::lock_guard<std::mutex> guard(transfer->mutex); transfer->canceled = true; }
    OnMain(CloseMain, transfer);
}
void WebImpl::ReleaseHandle(void* handle) {
    std::shared_ptr<Transfer> retained;
    {
        std::lock_guard<std::mutex> guard(m_storage->mutex);
        auto it=std::find_if(m_storage->transfers.begin(),m_storage->transfers.end(),
            [handle](const auto& item){return item.get()==handle;});
        if(it==m_storage->transfers.end()) std::abort();
        retained=*it;
        m_storage->transfers.erase(it);
    }
    { std::lock_guard<std::mutex> guard(retained->mutex); retained->canceled=true; }
    OnMain(CloseMain, retained.get());
}
Bool WebImpl::HasInternetConnection() const {
    return MAIN_THREAD_EM_ASM_INT({return typeof navigator !== 'undefined' && navigator.onLine !== false;});
}
Bool WebImpl::InitializeImplementation(const InitParams&) { return MainQueue()!=nullptr; }
void WebImpl::ShutdownImplementation() {}
}
