// GPU-thread-only cache of successful, immutable binding descriptors.
struct CachedGpuBinding {
    WGPUBindGroup group=nullptr;WGPUComputePipeline compute=nullptr;WGPURenderPipeline graphics=nullptr;
    std::vector<WGPUBindGroupEntry> entries;std::uint64_t stamp=0,bufferBytes=0;
    std::vector<WGPUTexture> textures;
    std::list<std::string>::iterator lru;
};
std::map<std::string,CachedGpuBinding> bindingCache;
std::list<std::string> bindingLru;
std::recursive_mutex bindingCacheMutex;
std::set<WGPUBindGroup> cachedBindingHandles;
// Reverse indexes so destroying one buffer/texture invalidates only the bindings that use it instead of
// scanning every cached binding on the main thread (previously ~0.18 ms per buffer destroy at 4096 entries).
std::unordered_map<WGPUBuffer,std::set<std::string>> bindingKeysByBuffer;
std::unordered_map<WGPUTexture,std::set<std::string>> bindingKeysByTexture;
template<class Index,class Handle>void UnindexBinding(Index& index,Handle handle,const std::string& key){
    auto found=index.find(handle);if(found==index.end())return;found->second.erase(key);if(found->second.empty())index.erase(found);
}
WGPUDevice bindingCacheDevice=nullptr;
std::uint64_t bindingStamp=0,bindingMisses=0,bindingBytes=0,bindingEvictions=0,bindingGeneration=0;
// [skperf] bindingBytes sums each cached group's bound range, so one big shared buffer bound by N groups counts N
// times (Street Kid bar: 4.7k groups -> '78 GB retained'). That is not memory. uniqueBindingBufferBytes counts each
// buffer once while any cached group references it (the cache's real extra retention). The eviction total also
// mixes LRU evictions with destroy invalidations; they are now published separately.
std::uint64_t uniqueBindingBufferBytes=0,bindingLruEvictions=0;
std::atomic<std::uint64_t> bindingHits{0},bindingWorkerHits{0},bindingWorkerReleases{0};
// [cmdlock] Budgets. Destroying a buffer or texture already erases every cached group that uses it
// (InvalidateGpuBindGroupsFor*), so the cache never extends a resource's life past its destroy; the 32 MiB "bound
// bytes" budget therefore protected no memory, yet refused every group binding a large storage buffer (262k refusals)
// and, with 4096 entries, the 32-vehicle working set no longer fit. ?bgBig=0 = old 32 MiB / 4096.
int BindingBigBudgets(){static const int v=MAIN_THREAD_EM_ASM_INT({return new URLSearchParams(location.search).get('bgBig')==='0'?0:1;});return v;}
#define bindingByteBudget (BindingBigBudgets()?(std::uint64_t(1)<<44):32ull*1024*1024)
#define bindingEntryBudget (BindingBigBudgets()?std::size_t(16384):std::size_t(4096))
EM_JS(void,PublishBindingCacheStats,(double hits,double misses,double bytes,int entries,double evictions,void* counters,void* workers,void* releases,double uniqueBytes,double lruEvictions),{
    const stats=Module['cp2077BindGroupCacheStats']={misses,retainedBufferBytes:uniqueBytes,boundRangeBytes:bytes,entries,evictions,lruEvictions,invalidations:evictions-lruEvictions};
    for(const [name,address] of [['hits',Number(counters)],['workerHits',Number(workers)],['workerReleases',Number(releases)]])
        Object.defineProperty(stats,name,{enumerable:true,get(){growMemViews();return Number(Atomics.load(HEAPU64,address/8));}});
});
void PublishBindings(){static std::atomic<unsigned> publishTick{0};if((publishTick.fetch_add(1)&255)!=0)return;std::lock_guard<std::recursive_mutex> guard(bindingCacheMutex);PublishBindingCacheStats(double(bindingHits.load()),double(bindingMisses),double(bindingBytes),int(bindingCache.size()),double(bindingEvictions),&bindingHits,&bindingWorkerHits,&bindingWorkerReleases,double(uniqueBindingBufferBytes),double(bindingLruEvictions));}
// [skperf] round 38: bumped on every erase (LRU eviction, buffer/texture invalidation, clear) so per-thread bind-group
// caches (ReuseBindingOnWorker) drop entries that the shared cache no longer vouches for.
std::atomic<std::uint64_t> bindingEpoch{1};
void EraseBinding(std::map<std::string,CachedGpuBinding>::iterator it){
    std::lock_guard<std::recursive_mutex> guard(bindingCacheMutex);
    bindingEpoch.fetch_add(1,std::memory_order_seq_cst);
    cachedBindingHandles.erase(it->second.group);
    auto& item=it->second;
    for(const auto& entry:item.entries)if(entry.buffer){
        auto found=bindingKeysByBuffer.find(entry.buffer);
        if(found!=bindingKeysByBuffer.end()&&found->second.erase(it->first)&&found->second.empty()){uniqueBindingBufferBytes-=wgpuBufferGetSize(entry.buffer);bindingKeysByBuffer.erase(found);}
    }
    for(auto texture:item.textures)UnindexBinding(bindingKeysByTexture,texture,it->first);wgpuBindGroupRelease(item.group);
    if(item.compute)wgpuComputePipelineRelease(item.compute);
    if(item.graphics)wgpuRenderPipelineRelease(item.graphics);
    for(const auto& entry:item.entries){if(entry.buffer)wgpuBufferRelease(entry.buffer);if(entry.textureView)wgpuTextureViewRelease(entry.textureView);if(entry.sampler)wgpuSamplerRelease(entry.sampler);}
    for(auto texture:item.textures)wgpuTextureRelease(texture);
    bindingLru.erase(item.lru);
    bindingBytes-=item.bufferBytes;bindingCache.erase(it);++bindingEvictions;
}
void TouchBinding(CachedGpuBinding& item){
    item.stamp=++bindingStamp;
    bindingLru.splice(bindingLru.end(),bindingLru,item.lru);
}
void ClearBindings(){std::lock_guard<std::recursive_mutex> guard(bindingCacheMutex);while(!bindingCache.empty())EraseBinding(bindingCache.begin());if(bindingCacheDevice)wgpuDeviceRelease(bindingCacheDevice);bindingCacheDevice=nullptr;bindingGeneration=0;PublishBindings();}
void SetBindingDevice(WGPUDevice device){std::lock_guard<std::recursive_mutex> guard(bindingCacheMutex);const auto generation=cp2077::web::GpuDeviceGeneration();if(bindingCacheDevice==device&&bindingGeneration==generation)return;ClearBindings();bindingCacheDevice=device;bindingGeneration=generation;wgpuDeviceAddRef(device);}
template<class T>void BindingKeyValue(std::string& key,T value){key.append(reinterpret_cast<const char*>(&value),sizeof(value));}
// Builds the key into key (cleared first; capacity kept). Leaves it empty when the request is not cacheable.
void BuildBindingKey(std::string& key,const Request& request){
    key.clear();if(request.encoder)return;
    BindingKeyValue(key,request.pipeline);BindingKeyValue(key,request.graphicsPipeline);BindingKeyValue(key,request.group);BindingKeyValue(key,request.count);
    for(std::size_t i=0;i<request.count;i++){
        const auto& e=request.entries[i];if(e.nextInChain){key.clear();return;}
        BindingKeyValue(key,e.binding);BindingKeyValue(key,e.buffer);BindingKeyValue(key,e.offset);BindingKeyValue(key,e.size);BindingKeyValue(key,e.sampler);BindingKeyValue(key,e.textureView);
    }
}
std::string BindingKey(const Request& request){std::string key;BuildBindingKey(key,request);return key;}
bool ReuseBinding(Request& request){
    std::lock_guard<std::recursive_mutex> guard(bindingCacheMutex);
    SetBindingDevice(request.device);request.bindingKey=BindingKey(request);if(request.bindingKey.empty())return false;
    auto it=bindingCache.find(request.bindingKey);
    if(it==bindingCache.end()){++bindingMisses;return false;}
    ++bindingHits;TouchBinding(it->second);request.created=it->second.group;wgpuBindGroupAddRef(request.created);
    request.encodedAt=request.checkedAt=CpProbeNow();PublishBindings();request.trace.CallEnd();if(request.context)emscripten_proxy_finish(request.context);return true;
}
// [cmdlock] Uncapped store-outcome counters, printed every 4096 stores: why a created group was not cached.
// 0 stored, 1 failed/duplicate/device, 2 nonzero-offset uniform, 3 view without parent, 4 single entry over the byte
// budget, 5 LRU evictions (entry cap), 6 LRU evictions (byte budget), 7 graphics/compute groups with an encoder (no key).
std::atomic<unsigned long long> bindingStoreOutcomes[8];
void NoteBindingStore(unsigned outcome){
    bindingStoreOutcomes[outcome].fetch_add(1,std::memory_order_relaxed);
    static std::atomic<unsigned long long> calls{0};
    if(outcome>4)return;
    if(((calls.fetch_add(1,std::memory_order_relaxed)+1)&4095)!=0)return;
    std::fprintf(stderr,"CP2077 cmdlock bind group store probe: stored=%llu dupOrFailed=%llu uniformOffset=%llu noViewParent=%llu overBytes=%llu evictCap=%llu evictBytes=%llu entries=%u bytesMB=%.1f\n",
        bindingStoreOutcomes[0].load(),bindingStoreOutcomes[1].load(),bindingStoreOutcomes[2].load(),bindingStoreOutcomes[3].load(),
        bindingStoreOutcomes[4].load(),bindingStoreOutcomes[5].load(),bindingStoreOutcomes[6].load(),unsigned(bindingCache.size()),double(bindingBytes)/1048576.0);
}
void StoreBinding(const Request& request){
    std::lock_guard<std::recursive_mutex> guard(bindingCacheMutex);
    if(request.failed||!request.created||request.bindingKey.empty()||request.device!=bindingCacheDevice||bindingCache.count(request.bindingKey)){NoteBindingStore(1);return;}
    // A graphics group that binds a uniform buffer at a nonzero static offset is an arena/ring slot of one draw
    // (auto-layout pair, or a uniform beyond the dynamic-offset limit): its key never repeats, and caching it
    // only evicted reusable groups and lengthened the main thread's hold on this mutex.
    // Same for compute groups of queued dispatches (perf7), whose varying uniforms are ring slots.
    if(request.graphicsPipeline||request.pipeline)
        for(std::size_t i=0;i<request.count;++i){
            const auto& e=request.entries[i];
            if(e.buffer&&e.offset&&(wgpuBufferGetUsage(e.buffer)&WGPUBufferUsage_Uniform)){NoteBindingStore(2);return;}
        }
    CachedGpuBinding item;item.group=request.created;item.compute=request.pipeline;item.graphics=request.graphicsPipeline;
    if(request.count)item.entries.assign(request.entries,request.entries+request.count);
    for(const auto& entry:item.entries)if(entry.textureView){
        auto parent=cp2077::web::GetGpuTextureViewParent(entry.textureView);
        if(!parent){
            NoteBindingStore(3);
            // [cmdlock] classify parentless views: first 48 distinct (binding, format, dimension, pipeline kind, entry count).
            static std::set<std::uint64_t> seenNoParent;
            WGPUTextureFormat format=WGPUTextureFormat_Undefined;WGPUTextureViewDimension dimension=WGPUTextureViewDimension_Undefined;
            const bool known=cp2077::web::GetGpuTextureViewInfo(entry.textureView,format,dimension);
            const std::uint64_t key=(std::uint64_t(entry.binding)<<40)|(std::uint64_t(format)<<16)|(std::uint64_t(dimension)<<8)|(request.graphicsPipeline?1:0)|(known?2:0);
            if(seenNoParent.size()<48&&seenNoParent.insert(key).second)
                std::fprintf(stderr,"CP2077 cmdlock parentless view: binding=%u format=%u dimension=%u known=%u graphics=%u entries=%u view=%p\n",
                    unsigned(entry.binding),unsigned(format),unsigned(dimension),unsigned(known),unsigned(request.graphicsPipeline!=nullptr),unsigned(request.count),(void*)entry.textureView);
            return;
        }
        if(std::find(item.textures.begin(),item.textures.end(),parent)==item.textures.end())item.textures.push_back(parent);
    }
    // Charge the bound range, not the whole buffer: pooled uniform arenas are shared by many bindings and are
    // retained by their pool anyway, so charging 256 KiB per binding starved the cache.
    for(const auto& e:item.entries)if(e.buffer){const auto whole=wgpuBufferGetSize(e.buffer);const auto bytes=(e.size&&e.size<whole)?e.size:whole;if(bytes>bindingByteBudget-item.bufferBytes){NoteBindingStore(4);return;}item.bufferBytes+=bytes;}
    while(bindingCache.size()>=bindingEntryBudget || bindingBytes+item.bufferBytes>bindingByteBudget){
        auto oldest=bindingLru.empty()?bindingCache.end():bindingCache.find(bindingLru.front());
        if(oldest==bindingCache.end())return;
        NoteBindingStore(bindingCache.size()>=bindingEntryBudget?5:6);EraseBinding(oldest);++bindingLruEvictions;
    }
    wgpuBindGroupAddRef(item.group);if(item.compute)wgpuComputePipelineAddRef(item.compute);if(item.graphics)wgpuRenderPipelineAddRef(item.graphics);
    for(const auto& e:item.entries){if(e.buffer)wgpuBufferAddRef(e.buffer);if(e.textureView)wgpuTextureViewAddRef(e.textureView);if(e.sampler)wgpuSamplerAddRef(e.sampler);}
    for(auto texture:item.textures)wgpuTextureAddRef(texture);
    item.stamp=++bindingStamp;bindingBytes+=item.bufferBytes;cachedBindingHandles.insert(item.group);
    bindingLru.push_back(request.bindingKey);item.lru=std::prev(bindingLru.end());
    for(const auto& e:item.entries)if(e.buffer){auto& keys=bindingKeysByBuffer[e.buffer];if(keys.empty())uniqueBindingBufferBytes+=wgpuBufferGetSize(e.buffer);keys.insert(request.bindingKey);}
    for(auto texture:item.textures)bindingKeysByTexture[texture].insert(request.bindingKey);
    bindingCache.emplace(request.bindingKey,std::move(item));PublishBindings();NoteBindingStore(0);
}
bool ReuseBindingOnWorker(Request& request){
    // The key is built before taking the shared mutex (it reads only the request) into a per-thread buffer that
    // keeps its capacity, so render workers no longer allocate and serialize on it. Callers that miss rebuild
    // request.bindingKey on the main thread (ReuseBinding), so it is not filled here.
    if(request.encoder||!cp2077::web::IsGpuDeviceReady())return false;
    thread_local std::string key;BuildBindingKey(key,request);if(key.empty())return false;
    // [skperf] round 38 bgLocal (round 39: default OFF, ?bgLocal=1 opt-in): a per-thread direct-mapped cache (128 entries) in front
    // of the shared one. A hit (same key, same device generation, no shared-cache erase since it was filled) takes no
    // bindingCacheMutex (one recursive mutex + std::map string lookup + LRU splice, taken by every recording worker
    // for every draw: tc25 showed the per-draw cost rising when two workers record at once). Each entry holds its own
    // reference; every 32nd local hit still goes through the shared cache so its LRU keeps hot groups.
    // tc26: lock time fell (bindGroup 1.73 -> 1.04 ms per big call) but FPS did not rise -> default OFF, ?bgLocal=1 opt-in.
    // [skperf] round 41: MAIN_THREAD_ (a render worker's own 'location' is the worker script URL, not the page's).
    static const int bgLocal=[]{const int v=MAIN_THREAD_EM_ASM_INT({try{return new URLSearchParams(location.search).get('bgLocal')==='1'?1:0;}catch(e){return 0;}});
        std::fprintf(stderr,"CP2077 skperf switches: bgLocal=%d\n",v);return v;}();
    if(!bgLocal){ // [skperf] round 41: default path = exactly the pre-round-38 code (no epoch/generation reads, no thread_local array)
        std::lock_guard<std::recursive_mutex> guard(bindingCacheMutex);
        if(!bindingCacheDevice||bindingGeneration!=cp2077::web::GpuDeviceGeneration())return false;
        auto it=bindingCache.find(key);if(it==bindingCache.end())return false;
        request.created=it->second.group;wgpuBindGroupAddRef(request.created);TouchBinding(it->second);++bindingHits;++bindingWorkerHits;return true;
    }
    struct LocalBinding{std::string key;WGPUBindGroup group=nullptr;std::uint64_t epoch=0,generation=0;};
    thread_local LocalBinding local[128];thread_local unsigned localHits=0;
    const std::uint64_t epoch=bindingEpoch.load(std::memory_order_seq_cst),generation=cp2077::web::GpuDeviceGeneration();
    LocalBinding* slot=bgLocal?&local[std::hash<std::string>{}(key)&127]:nullptr;
    if(slot&&slot->group&&slot->epoch==epoch&&slot->generation==generation&&slot->key==key&&(++localHits&31)!=0){
        wgpuBindGroupAddRef(slot->group);request.created=slot->group;return true;
    }
    WGPUBindGroup previous=nullptr;
    {
        std::lock_guard<std::recursive_mutex> guard(bindingCacheMutex);
        if(!bindingCacheDevice||bindingGeneration!=cp2077::web::GpuDeviceGeneration())return false;
        auto it=bindingCache.find(key);if(it==bindingCache.end())return false;
        request.created=it->second.group;wgpuBindGroupAddRef(request.created);TouchBinding(it->second);++bindingHits;++bindingWorkerHits;
        if(slot&&slot->group!=request.created){previous=slot->group;slot->key=key;slot->group=request.created;wgpuBindGroupAddRef(slot->group);}
        if(slot){slot->epoch=epoch;slot->generation=generation;}
    }
    if(previous)cp2077::web::ReleaseGpuBindGroup(previous); // outside the lock; routes to the main thread when not cached
    return true;
}
bool ReleaseCachedBindingOnWorker(WGPUBindGroup group){
    std::lock_guard<std::recursive_mutex> guard(bindingCacheMutex);
    if(!cachedBindingHandles.count(group))return false;
    wgpuBindGroupRelease(group);++bindingWorkerReleases;return true;
}
