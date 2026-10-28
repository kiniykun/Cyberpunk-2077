#pragma once
#include <array>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace recCore { namespace ioLayer { namespace web {
// Preserve the recorder's three outstanding operations and caller-owned buffers.
// Cancellation drains an already-running POSIX operation before releasing them.
class AsyncFileImpl {
    struct Operation {
        std::thread worker;
        std::atomic<bool> done{false};
        bool reserved=false;
        AsyncOpResult result{EAsyncResultCode::FAILED,0};
    };
    std::array<Operation,3> operations;
    int fd=-1;
    std::mutex appendMutex;
    AsyncOpResult Transfer(bool write,void* buffer,Uint32 size,Uint32 offset,bool append) {
        AsyncOpResult result{EAsyncResultCode::SUCCESS,0};
        if(fd<0 || (!buffer && size)) return {EAsyncResultCode::FAILED,0};
        std::unique_lock<std::mutex> appendLock(appendMutex,std::defer_lock);
        off_t position=offset;
        if(append) {
            appendLock.lock();
            struct stat st;
            if(fstat(fd,&st)!=0) return {EAsyncResultCode::FAILED,0};
            position=st.st_size;
        }
        while(result.m_bytesTransferred<size) {
            Uint32 transferred=result.m_bytesTransferred;
            ssize_t n=write ? pwrite(fd,static_cast<char*>(buffer)+transferred,size-transferred,position+transferred)
                            : pread(fd,static_cast<char*>(buffer)+transferred,size-transferred,position+transferred);
            if(n<0 && errno==EINTR) continue;
            if(n<0 || (write && n==0)) {result.m_resultCode=EAsyncResultCode::FAILED;break;}
            if(n==0) break; // EOF is a successful short read, as in the native API.
            result.m_bytesTransferred+=static_cast<Uint32>(n);
        }
        return result;
    }
    AsyncOpHandle Start(bool write,void* buffer,Uint32 size,Uint32 offset,bool append) {
        if(fd<0 || (!buffer && size)) return INVALID_ASYNC_OP_HANDLE;
        for(AsyncOpHandle i=0;i<3;++i) {
            auto& op=operations[i];
            if(op.reserved) continue;
            op.reserved=true;op.done.store(false,std::memory_order_relaxed);
            op.worker=std::thread([this,&op,write,buffer,size,offset,append] {
                op.result=Transfer(write,buffer,size,offset,append);
                op.done.store(true,std::memory_order_release);
            });
            return i;
        }
        return INVALID_ASYNC_OP_HANDLE;
    }
public:
    AsyncFileImpl()=default;
    ~AsyncFileImpl() {CloseFile();}
    Bool OpenFile(const AnsiChar* name,EOpenFileMode mode) {
        if(fd>=0 || !name) return false;
        fd=open(name,mode==EOpenFileMode::READ ? O_RDONLY : O_WRONLY|O_CREAT|O_EXCL,0600);
        return fd>=0;
    }
    void CloseFile() {
        for(AsyncOpHandle i=0;i<3;++i) if(operations[i].reserved) {auto handle=i;CancelOperation(handle);}
        if(fd>=0) {close(fd);fd=-1;}
    }
    Uint32 GetFileSize() const {struct stat st;return fd>=0 && fstat(fd,&st)==0 ? static_cast<Uint32>(st.st_size) : 0;}
    Bool WriteSync(const void* buffer,Uint32 size,Uint32 offset) {auto r=Transfer(true,const_cast<void*>(buffer),size,offset,false);return static_cast<Bool>(r) && r.m_bytesTransferred==size;}
    Bool WriteAtEndSync(const void* buffer,Uint32 size) {auto r=Transfer(true,const_cast<void*>(buffer),size,0,true);return static_cast<Bool>(r) && r.m_bytesTransferred==size;}
    Bool ReadSync(void* buffer,Uint32 size,Uint32 offset,Uint32& bytes) {auto r=Transfer(false,buffer,size,offset,false);bytes=r.m_bytesTransferred;return static_cast<Bool>(r);}
    AsyncOpHandle StartWriteAsync(const void* buffer,Uint32 size,Uint32 offset) {return Start(true,const_cast<void*>(buffer),size,offset,offset==0xffffffffu);}
    AsyncOpHandle StartWriteAtTheEndAsync(const void* buffer,Uint32 size) {return Start(true,const_cast<void*>(buffer),size,0,true);}
    AsyncOpHandle StartReadAsync(void* buffer,Uint32 size,Uint32 offset) {return Start(false,buffer,size,offset,false);}
    AsyncOpResult GetAsyncOpResult(AsyncOpHandle& handle,Bool wait=false) {
        if(handle<0 || handle>=3 || !operations[handle].reserved) return {EAsyncResultCode::FAILED,0};
        auto& op=operations[handle];
        if(!wait && !op.done.load(std::memory_order_acquire)) return {EAsyncResultCode::PENDING,0};
        op.worker.join();auto result=op.result;op.reserved=false;handle=INVALID_ASYNC_OP_HANDLE;return result;
    }
    void CancelOperation(AsyncOpHandle& handle) {GetAsyncOpResult(handle,true);}
};
}}}
