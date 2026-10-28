// Creation and callbacks stay on the browser GPU thread. Workers may retain
// exact validated cache hits while holding the shared cache mutex.
// Cache ownership is separate from each submitted command's pipeline reference.
struct CachedRenderPipeline {
    WGPURenderPipeline pipeline=nullptr;
    WGPUShaderModule vertex=nullptr,fragment=nullptr;
    WGPUPipelineLayout layout=nullptr;
    std::uint64_t stamp=0;
    std::string key; // full key; the map is keyed by its 64-bit hash
};
std::unordered_map<std::uint64_t,CachedRenderPipeline> renderPipelineCache;
std::recursive_mutex renderPipelineCacheMutex;
std::set<WGPURenderPipeline> cachedRenderPipelineHandles;
// Negative cache: descriptors whose creation failed WebGPU validation on a live device are not retried every
// draw (each retry re-validated, logged and failed again). Entries hold references to the modules/layout their
// key embeds by address, so a released handle cannot be reused for a different object under the same key.
struct FailedRenderPipeline {
    WGPUShaderModule vertex=nullptr,fragment=nullptr;
    WGPUPipelineLayout layout=nullptr;
    std::string key;
};
std::unordered_map<std::uint64_t,FailedRenderPipeline> failedRenderPipelines;
std::uint64_t failedRenderPipelineHits=0;
std::map<std::string,std::vector<RenderPipelineRequest*>> pendingRenderPipelines;
WGPUDevice renderPipelineCacheDevice=nullptr;
std::uint64_t pipelineStamp=0,pipelineMisses=0,pipelineJoined=0,renderPipelineCacheGeneration=0;
std::atomic<std::uint64_t> pipelineHits{0},pipelineWorkerHits{0};
std::atomic<std::uint64_t> pipelineWorkerReleases{0};
double pipelineCompileMs=0;
EM_JS(void,PublishPipelineCacheStats,(double hits,double misses,double joined,double compileMs,int entries,void* hitCounter,void* workerCounter,void* releaseCounter,void* asyncCounter,void* skipCounter),{
    const stats=Module['cp2077PipelineCacheStats'] ||= {};
    Object.assign(stats,{misses,joined,compileMs,entries});
    for(const [name,address] of [['hits',Number(hitCounter)],['workerHits',Number(workerCounter)],['workerReleases',Number(releaseCounter)],['asyncStarted',Number(asyncCounter)],['pendingSkips',Number(skipCounter)]])
        Object.defineProperty(stats,name,{configurable:true,enumerable:true,get(){growMemViews();return Number(Atomics.load(HEAPU64,address/8));}});
});
void UpdatePipelineCacheStats(){
    std::lock_guard<std::recursive_mutex> guard(renderPipelineCacheMutex);
    PublishPipelineCacheStats(double(pipelineHits.load()),double(pipelineMisses),double(pipelineJoined),pipelineCompileMs,int(renderPipelineCache.size()),&pipelineHits,&pipelineWorkerHits,&pipelineWorkerReleases,&pipelineAsyncStarted,&pipelinePendingSkips);
}
template<class T> void PipelineKeyValue(std::string& key,T value){
    key.append(reinterpret_cast<const char*>(&value),sizeof(value));
}
// Same bytes OwnString would keep: a null view contributes an empty string.
void PipelineKeyView(std::string& key,const WGPUStringView& value){
    const std::size_t size=!value.data?0:value.length==WGPU_STRLEN?std::strlen(value.data):value.length;
    PipelineKeyValue(key,std::uint64_t(size));if(size)key.append(value.data,size);
}
void PipelineKeyStencil(std::string& key,const WGPUStencilFaceState& state){
    PipelineKeyValue(key,state.compare);PipelineKeyValue(key,state.failOp);
    PipelineKeyValue(key,state.depthFailOp);PipelineKeyValue(key,state.passOp);
}
void PipelineKeyBlend(std::string& key,const WGPUBlendComponent& state){
    PipelineKeyValue(key,state.operation);PipelineKeyValue(key,state.srcFactor);PipelineKeyValue(key,state.dstFactor);
}
// Reads either the caller's descriptor or the request's owned copy (same bytes),
// so worker hits skip CopyRenderDescriptor. Callers validate the descriptor first.
void BuildRenderPipelineKey(std::string& key,WGPUDevice device,std::uint64_t generation,const WGPURenderPipelineDescriptor& d){
    key.clear();
    PipelineKeyValue(key,reinterpret_cast<std::uintptr_t>(device));
    PipelineKeyValue(key,generation);
    PipelineKeyValue(key,reinterpret_cast<std::uintptr_t>(d.layout));
    PipelineKeyValue(key,reinterpret_cast<std::uintptr_t>(d.vertex.module));
    PipelineKeyValue(key,bool(d.vertex.entryPoint.data));PipelineKeyView(key,d.vertex.entryPoint);
    PipelineKeyValue(key,std::uint32_t(d.vertex.bufferCount));
    for(std::size_t j=0;j<d.vertex.bufferCount;++j){
        const auto& b=d.vertex.buffers[j];
        PipelineKeyValue(key,b.arrayStride);PipelineKeyValue(key,b.stepMode);
        PipelineKeyValue(key,std::uint32_t(b.attributeCount));
        for(std::size_t i=0;i<b.attributeCount;++i){
            const auto& a=b.attributes[i];PipelineKeyValue(key,a.format);
            PipelineKeyValue(key,a.offset);PipelineKeyValue(key,a.shaderLocation);
        }
    }
    PipelineKeyValue(key,d.primitive.topology);PipelineKeyValue(key,d.primitive.stripIndexFormat);
    PipelineKeyValue(key,d.primitive.frontFace);PipelineKeyValue(key,d.primitive.cullMode);
    PipelineKeyValue(key,d.primitive.unclippedDepth);
    PipelineKeyValue(key,bool(d.depthStencil));
    if(d.depthStencil){
        const auto& s=*d.depthStencil;PipelineKeyValue(key,s.format);
        PipelineKeyValue(key,s.depthWriteEnabled);PipelineKeyValue(key,s.depthCompare);
        PipelineKeyStencil(key,s.stencilFront);PipelineKeyStencil(key,s.stencilBack);
        PipelineKeyValue(key,s.stencilReadMask);PipelineKeyValue(key,s.stencilWriteMask);
        PipelineKeyValue(key,s.depthBias);PipelineKeyValue(key,s.depthBiasSlopeScale);PipelineKeyValue(key,s.depthBiasClamp);
    }
    PipelineKeyValue(key,d.multisample.count);PipelineKeyValue(key,d.multisample.mask);
    PipelineKeyValue(key,d.multisample.alphaToCoverageEnabled);
    PipelineKeyValue(key,bool(d.fragment));
    if(d.fragment){
        PipelineKeyValue(key,reinterpret_cast<std::uintptr_t>(d.fragment->module));
        PipelineKeyValue(key,bool(d.fragment->entryPoint.data));PipelineKeyView(key,d.fragment->entryPoint);
        PipelineKeyValue(key,std::uint32_t(d.fragment->targetCount));
        for(std::size_t i=0;i<d.fragment->targetCount;++i){
            const auto& t=d.fragment->targets[i];
            PipelineKeyValue(key,t.format);PipelineKeyValue(key,t.writeMask);PipelineKeyValue(key,bool(t.blend));
            if(t.blend){PipelineKeyBlend(key,t.blend->color);PipelineKeyBlend(key,t.blend->alpha);}
        }
    }
}
std::uint64_t HashRenderPipelineKey(const std::string& key){
    std::uint64_t h=0xcbf29ce484222325ull^key.size();std::size_t i=0;
    for(;i+8<=key.size();i+=8){
        std::uint64_t word;std::memcpy(&word,key.data()+i,8);
        h=(h^word)*0x100000001b3ull;h^=h>>29;
    }
    for(;i<key.size();++i)h=(h^std::uint8_t(key[i]))*0x100000001b3ull;
    h^=h>>33;h*=0xff51afd7ed558ccdull;h^=h>>33;h*=0xc4ceb9fe1a85ec53ull;h^=h>>33;
    return h;
}
// Per-thread scratch key keeps its capacity, so cache hits do not allocate.
std::string& RenderPipelineScratchKey(){
    thread_local std::string key;return key;
}
CachedRenderPipeline* FindCachedRenderPipeline(const std::string& key,std::uint64_t hash){
    auto found=renderPipelineCache.find(hash);
    return found!=renderPipelineCache.end() && found->second.key==key ? &found->second : nullptr;
}
void ReleaseCachedRenderPipeline(CachedRenderPipeline& entry){
    std::lock_guard<std::recursive_mutex> guard(renderPipelineCacheMutex);
    cachedRenderPipelineHandles.erase(entry.pipeline);
    wgpuRenderPipelineRelease(entry.pipeline);wgpuShaderModuleRelease(entry.vertex);
    if(entry.fragment)wgpuShaderModuleRelease(entry.fragment);
    if(entry.layout)wgpuPipelineLayoutRelease(entry.layout);
}
void ReleaseFailedRenderPipelines(){
    std::lock_guard<std::recursive_mutex> guard(renderPipelineCacheMutex);
    for(auto& item:failedRenderPipelines){
        auto& entry=item.second;
        wgpuShaderModuleRelease(entry.vertex);
        if(entry.fragment)wgpuShaderModuleRelease(entry.fragment);
        if(entry.layout)wgpuPipelineLayoutRelease(entry.layout);
    }
    failedRenderPipelines.clear();
}
// Caller holds renderPipelineCacheMutex.
bool IsFailedRenderPipeline(const std::string& key,std::uint64_t hash){
    auto found=failedRenderPipelines.find(hash);
    if(found==failedRenderPipelines.end() || found->second.key!=key)return false;
    const auto hits=++failedRenderPipelineHits;
    if((hits&(hits-1))==0)
        std::fprintf(stderr,"CP2077 WebGPU render pipeline negative-cache hits=%llu entries=%u\n",
                     (unsigned long long)hits,unsigned(failedRenderPipelines.size()));
    return true;
}
void SelectPipelineCacheDevice(WGPUDevice device){
    std::lock_guard<std::recursive_mutex> guard(renderPipelineCacheMutex);
    const auto generation=cp2077::web::GpuDeviceGeneration();
    if(device==renderPipelineCacheDevice && generation==renderPipelineCacheGeneration)return;
    for(auto& item:renderPipelineCache)ReleaseCachedRenderPipeline(item.second);
    renderPipelineCache.clear();
    ReleaseFailedRenderPipelines();
    if(renderPipelineCacheDevice)wgpuDeviceRelease(renderPipelineCacheDevice);
    renderPipelineCacheDevice=device;renderPipelineCacheGeneration=generation;wgpuDeviceAddRef(device);
}
bool TryReuseRenderPipeline(RenderPipelineRequest& request){
    std::lock_guard<std::recursive_mutex> guard(renderPipelineCacheMutex);
    SelectPipelineCacheDevice(request.device);
    auto& key=RenderPipelineScratchKey();
    BuildRenderPipelineKey(key,request.device,request.deviceGeneration,request.descriptor);
    const auto hash=HashRenderPipelineKey(key);
    if(auto* found=FindCachedRenderPipeline(key,hash)){
        request.pipeline=found->pipeline;wgpuRenderPipelineAddRef(request.pipeline);
        found->stamp=++pipelineStamp;++pipelineHits;UpdatePipelineCacheStats();
        emscripten_proxy_finish(request.context);return true;
    }
    if(IsFailedRenderPipeline(key,hash)){
        request.pipeline=nullptr;emscripten_proxy_finish(request.context);return true;
    }
    request.cacheKey=key;request.cacheHash=hash;
    auto pending=pendingRenderPipelines.find(request.cacheKey);
    if(pending!=pendingRenderPipelines.end()){
        pending->second.push_back(&request);++pipelineJoined;UpdatePipelineCacheStats();return true;
    }
    pendingRenderPipelines.emplace(request.cacheKey,std::vector<RenderPipelineRequest*>{});
    ++pipelineMisses;request.compileStarted=emscripten_get_now();UpdatePipelineCacheStats();return false;
}
void PublishCompletedRenderPipeline(RenderPipelineRequest& request){
    std::lock_guard<std::recursive_mutex> guard(renderPipelineCacheMutex);
    if(!request.warm)pipelineCompileMs+=emscripten_get_now()-request.compileStarted; // [warmup] draw-path compile time only
    if(request.pipeline && !request.warm)RecordCompletedRenderPipeline(request); // [warmup] ?psoRecord=1
    // A 64-bit hash collision with a different live key leaves that entry in place.
    if(request.pipeline && !renderPipelineCache.count(request.cacheHash)){
        // Bounded LRU: submitted commands keep their own references on eviction.
        if(renderPipelineCache.size()>=2048){
            auto oldest=std::min_element(renderPipelineCache.begin(),renderPipelineCache.end(),
                [](const auto& a,const auto& b){return a.second.stamp<b.second.stamp;});
            ReleaseCachedRenderPipeline(oldest->second);renderPipelineCache.erase(oldest);
        }
        CachedRenderPipeline entry{request.pipeline,request.descriptor.vertex.module,
            request.descriptor.fragment?request.fragment.module:nullptr,request.descriptor.layout,++pipelineStamp,request.cacheKey};
        wgpuRenderPipelineAddRef(entry.pipeline);wgpuShaderModuleAddRef(entry.vertex);
        if(entry.fragment)wgpuShaderModuleAddRef(entry.fragment);
        if(entry.layout)wgpuPipelineLayoutAddRef(entry.layout);
        cachedRenderPipelineHandles.insert(entry.pipeline);
        renderPipelineCache.emplace(request.cacheHash,std::move(entry));
    }
    // Only a validation failure on the device/generation it was keyed for is deterministic; device loss,
    // OOM and internal errors stay retryable.
    if(!request.pipeline && request.validationFailed && cp2077::web::IsGpuDeviceReady() &&
       cp2077::web::MainThreadGpuDevice()==request.device &&
       cp2077::web::GpuDeviceGeneration()==request.deviceGeneration &&
       request.device==renderPipelineCacheDevice && !failedRenderPipelines.count(request.cacheHash)){
        if(failedRenderPipelines.size()>=512)ReleaseFailedRenderPipelines();
        FailedRenderPipeline entry{request.descriptor.vertex.module,
            request.descriptor.fragment?request.fragment.module:nullptr,request.descriptor.layout,request.cacheKey};
        wgpuShaderModuleAddRef(entry.vertex);
        if(entry.fragment)wgpuShaderModuleAddRef(entry.fragment);
        if(entry.layout)wgpuPipelineLayoutAddRef(entry.layout);
        failedRenderPipelines.emplace(request.cacheHash,std::move(entry));
        static unsigned reported=0;
        if(reported++<32)std::fprintf(stderr,"CP2077 WebGPU render pipeline failure cached (no retry): label=%s entries=%u\n",
                                      request.label.c_str(),unsigned(failedRenderPipelines.size()));
    }
    auto pending=pendingRenderPipelines.find(request.cacheKey);
    if(pending!=pendingRenderPipelines.end()){
        auto waiters=std::move(pending->second);pendingRenderPipelines.erase(pending);
        for(auto* waiter:waiters){
            waiter->pipeline=request.pipeline;
            if(waiter->pipeline)wgpuRenderPipelineAddRef(waiter->pipeline);
            emscripten_proxy_finish(waiter->context);
        }
    }
    UpdatePipelineCacheStats();
}
// Worker fast path on the caller's validated descriptor, before any copy.
// The key is built and hashed outside the shared mutex (every draw on every render worker comes here, and the
// retirement worker takes the same mutex once per retired draw); the device/generation it embeds are read
// under a short lock and re-checked when the lock is taken again for the lookup.
bool TryReuseRenderPipelineOnWorker(const WGPURenderPipelineDescriptor& descriptor,WGPURenderPipeline& pipeline){
    if(!cp2077::web::IsGpuDeviceReady())return false;
    WGPUDevice device=nullptr;std::uint64_t generation=0;
    {
        std::lock_guard<std::recursive_mutex> guard(renderPipelineCacheMutex);
        if(!renderPipelineCacheDevice || renderPipelineCacheGeneration!=cp2077::web::GpuDeviceGeneration())return false;
        device=renderPipelineCacheDevice;generation=renderPipelineCacheGeneration;
    }
    auto& key=RenderPipelineScratchKey();
    BuildRenderPipelineKey(key,device,generation,descriptor);
    const auto hash=HashRenderPipelineKey(key);
    std::lock_guard<std::recursive_mutex> guard(renderPipelineCacheMutex);
    if(renderPipelineCacheDevice!=device || renderPipelineCacheGeneration!=generation)return false;
    auto* found=FindCachedRenderPipeline(key,hash);
    if(!found){
        if(!IsFailedRenderPipeline(key,hash))return false;
        pipeline=nullptr;return true; // known validation failure: the caller skips the draw
    }
    pipeline=found->pipeline;wgpuRenderPipelineAddRef(pipeline);
    found->stamp=++pipelineStamp;++pipelineHits;++pipelineWorkerHits;
    return true;
}
bool ReleaseCachedRenderPipelineOnWorker(WGPURenderPipeline pipeline){
    std::lock_guard<std::recursive_mutex> guard(renderPipelineCacheMutex);
    if(!cachedRenderPipelineHandles.count(pipeline))return false;
    wgpuRenderPipelineRelease(pipeline);++pipelineWorkerReleases;return true;
}
