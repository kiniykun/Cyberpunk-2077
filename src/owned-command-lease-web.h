#pragma once
#include <atomic>
#include <mutex>
#include <condition_variable>
// Registry mutex is held when a lease is acquired. An owned command may not be
// erased/submitted until its owning thread unbinds it. Registry rehash preserves
// references. Teardown additionally waits for leases before swapping the map.
// [cmdlock] active/closing are atomic so a lease release does not re-take the registry mutex (it did on every
// draw/dispatch). closing is only set under the registry mutex; BeginClose re-checks active under that mutex, and
// the last releaser takes the mutex before notifying when closing is set, so no wakeup is missed (seq_cst).
struct OwnedCommandOperations {
    std::atomic<unsigned> active{0};std::atomic<bool> closing{false};
    std::condition_variable idle;
    bool BeginClose(std::unique_lock<std::mutex>& lock,bool browserMain){
        if(browserMain&&active.load())return false; // Never block callback delivery.
        closing.store(true);idle.wait(lock,[&]{return active.load()==0;});return true;
    }
};
class OwnedCommandLease {
    OwnedCommandOperations& operations;std::mutex& mutex;bool held=false;
public:
    OwnedCommandLease(OwnedCommandOperations& state,std::mutex& registry):operations(state),mutex(registry){
        if(!operations.closing.load()){operations.active.fetch_add(1);held=true;}
    }
    explicit operator bool()const{return held;}
    ~OwnedCommandLease(){
        if(!held)return;
        if(operations.active.fetch_sub(1)==1&&operations.closing.load()){std::lock_guard<std::mutex> lock(mutex);operations.idle.notify_all();}
    }
    OwnedCommandLease(const OwnedCommandLease&)=delete;
    OwnedCommandLease& operator=(const OwnedCommandLease&)=delete;
};
