#pragma once
#include <memory>
namespace http {
class AsyncClient;
class AsyncRequest;
class WebImpl {
    RED_USE_MEMORY_POOL(PoolHTTP);
public:
    explicit WebImpl(AsyncClient& client);
    ~WebImpl();
    void Update();
    Bool StartExecuteRequest(AsyncRequest* request);
    void AbortExecute(void* handle);
    void ReleaseHandle(void* handle);
    Bool HasInternetConnection() const;
    static Bool InitializeImplementation(const struct InitParams& params);
    static void ShutdownImplementation();
private:
    struct Storage;
    std::unique_ptr<Storage> m_storage;
};
}
