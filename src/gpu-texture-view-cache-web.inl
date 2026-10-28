// Creation stays on the GPU owner. Exact validated hits and cached leases may
// be retained/released on workers under the shared cache mutex.
struct CachedTextureView {WGPUTexture texture=nullptr;WGPUTextureView view=nullptr;std::uint64_t stamp=0;};
std::map<std::string,CachedTextureView> textureViewCache;
std::recursive_mutex textureViewMutex;
// perf11: reader/writer lock for the worker hit path. Only the main thread mutates textureViewCache,
// cachedTextureViewHandles, textureViewCacheDevice and textureViewGeneration, and always takes this exclusively
// (briefly, while also holding textureViewMutex) to do so; render workers only read them under a shared lock, so
// ~65k worker hits/s no longer serialize on one mutex. Worker hits no longer refresh the LRU stamp (the cache stays
// far below its 4096-entry cap; capacityEvictions was 0). Lock order: textureViewMutex -> textureViewRw.
std::shared_mutex textureViewRw;
std::set<WGPUTextureView> cachedTextureViewHandles;
WGPUDevice textureViewCacheDevice=nullptr;
// Metadata does not retain textures or views. Live cached bind groups retain
// their view handles, preventing handle reuse while their parent is recorded.
// Unknown parents are excluded from binding caching rather than guessed.
std::map<WGPUTextureView,WGPUTexture> textureViewParents;
// [skperf] Reverse indexes (guarded by textureViewMutex). InvalidateGpuTextureViews used to walk the whole view
// cache AND the whole parent map (it only shrinks when a texture is destroyed, so it grows to tens of thousands of
// entries) on the main thread for EVERY texture destroy. Indoor openings destroy ~500-1400 textures/s (Street Kid
// bar, Corpo corridor), so that scan grew with session time and starved the GPU-owner thread. Entries may be stale
// (a view handle re-registered under another texture); erasure always re-checks the parent map.
// ?viewIndex=0 = old full scans (the indexes are still maintained).
std::unordered_map<WGPUTexture,std::vector<WGPUTextureView>> parentViewsByTexture;
std::unordered_map<WGPUTexture,std::vector<std::string>> cacheKeysByTexture;
bool ViewIndexOn(){static const int v=MAIN_THREAD_EM_ASM_INT({return new URLSearchParams(location.search).get('viewIndex')==='0'?0:1;});return v!=0;}
void SetViewParentLocked(WGPUTextureView view,WGPUTexture texture){
 auto found=textureViewParents.find(view);
 if(found!=textureViewParents.end()&&found->second==texture)return;
 textureViewParents[view]=texture;parentViewsByTexture[texture].push_back(view);
}
void UnindexCachedViewKey(WGPUTexture texture,const std::string& key){
 auto found=cacheKeysByTexture.find(texture);if(found==cacheKeysByTexture.end())return;
 auto& keys=found->second;auto it=std::find(keys.begin(),keys.end(),key);
 if(it!=keys.end()){*it=std::move(keys.back());keys.pop_back();}
 if(keys.empty())cacheKeysByTexture.erase(found);
}
std::uint64_t textureViewStamp=0,textureViewMisses=0,textureViewEvictions=0,textureViewGeneration=0;
std::atomic<std::uint64_t> textureViewHits{0},textureViewWorkerHits{0},textureViewWorkerReleases{0};
std::uint64_t textureViewCapacityEvictions=0,textureViewInvalidations=0,textureViewDeviceClears=0;
// View descriptors are small; this retains handles, not duplicate image data.
// Destruction still invalidates every view for the destroyed texture immediately.
constexpr std::size_t textureViewEntryLimit=4096;
EM_JS(void,PublishTextureViewCache,(double hits,double misses,int entries,double evictions,double capacity,double invalidations,double clears,void* counters,void* workerCounters,void* releases),{
 const stats=Module['cp2077TextureViewCacheStats']={misses,entries,evictions,capacityEvictions:capacity,textureInvalidations:invalidations,deviceClears:clears,entryLimit:4096};
 for(const [name,address] of [['hits',Number(counters)],['workerHits',Number(workerCounters)],['workerReleases',Number(releases)]])
  Object.defineProperty(stats,name,{enumerable:true,get(){growMemViews();return Number(Atomics.load(HEAPU64,address/8));}});
});
void PublishViews(){static std::atomic<unsigned> publishTick{0};if((publishTick.fetch_add(1)&255)!=0)return;std::lock_guard<std::recursive_mutex> guard(textureViewMutex);PublishTextureViewCache(double(textureViewHits.load()),double(textureViewMisses),int(textureViewCache.size()),double(textureViewEvictions),double(textureViewCapacityEvictions),double(textureViewInvalidations),double(textureViewDeviceClears),&textureViewHits,&textureViewWorkerHits,&textureViewWorkerReleases);}
void EraseView(std::map<std::string,CachedTextureView>::iterator it,unsigned reason=1){std::lock_guard<std::recursive_mutex> guard(textureViewMutex);const auto erasedView=it->second.view;const auto erasedTexture=it->second.texture;UnindexCachedViewKey(erasedTexture,it->first);{std::unique_lock<std::shared_mutex> rw(textureViewRw);cachedTextureViewHandles.erase(erasedView);textureViewCache.erase(it);}wgpuTextureViewRelease(erasedView);wgpuTextureRelease(erasedTexture);++textureViewEvictions;if(reason==0)++textureViewCapacityEvictions;else if(reason==1)++textureViewInvalidations;else ++textureViewDeviceClears;}
void ClearViews(){std::lock_guard<std::recursive_mutex> guard(textureViewMutex);while(!textureViewCache.empty())EraseView(textureViewCache.begin(),2);textureViewParents.clear();parentViewsByTexture.clear();cacheKeysByTexture.clear();WGPUDevice oldDevice=nullptr;{std::unique_lock<std::shared_mutex> rw(textureViewRw);oldDevice=textureViewCacheDevice;textureViewCacheDevice=nullptr;textureViewGeneration=0;}if(oldDevice)wgpuDeviceRelease(oldDevice);PublishViews();}
template<class T>void ViewKeyValue(std::string& key,T value){key.append(reinterpret_cast<const char*>(&value),sizeof(value));}
// perf11: builds into key (cleared first, capacity kept) so the worker path can use a thread_local buffer outside
// textureViewMutex; empty when not cacheable.
void BuildTextureViewKey(std::string& key,const ViewRequest& request){
 key.clear();const auto& d=request.descriptor;if(d.nextInChain)return;
 ViewKeyValue(key,request.texture);ViewKeyValue(key,d.format);ViewKeyValue(key,d.dimension);ViewKeyValue(key,d.baseMipLevel);ViewKeyValue(key,d.mipLevelCount);ViewKeyValue(key,d.baseArrayLayer);ViewKeyValue(key,d.arrayLayerCount);ViewKeyValue(key,d.aspect);ViewKeyValue(key,d.usage);
 ViewKeyValue(key,bool(d.label.data));
 if(d.label.data){const auto length=d.label.length==WGPU_STRLEN?std::strlen(d.label.data):d.label.length;ViewKeyValue(key,length);key.append(d.label.data,length);}
}
std::string TextureViewKey(const ViewRequest& request){std::string key;BuildTextureViewKey(key,request);return key;}
bool ReuseTextureView(ViewRequest& request){
 std::lock_guard<std::recursive_mutex> guard(textureViewMutex);
 // [cmdlock] The very first miss used to run ClearViews too (cache device still null), which also wiped
 // textureViewParents. The multilayer pool registers its first chunk/dummy views at boot, before that first miss, so
 // they lost their parents for the whole session and every multilayer bind group was refused by the bind-group cache
 // (binding 640 'parentless', ~300 groups recreated per frame at 32 vehicles). Only a real device change clears now.
 if(textureViewCacheDevice!=request.device || textureViewGeneration!=request.generation){if(textureViewCacheDevice)ClearViews();{std::unique_lock<std::shared_mutex> rw(textureViewRw);textureViewCacheDevice=request.device;textureViewGeneration=request.generation;}wgpuDeviceAddRef(request.device);}
 request.key=TextureViewKey(request);if(request.key.empty())return false;
 auto it=textureViewCache.find(request.key);if(it==textureViewCache.end()){++textureViewMisses;return false;}
 ++textureViewHits;it->second.stamp=++textureViewStamp;request.view=it->second.view;wgpuTextureViewAddRef(request.view);PublishViews();emscripten_proxy_finish(request.context);return true;
}
void StoreTextureView(const ViewRequest& request){
 std::lock_guard<std::recursive_mutex> guard(textureViewMutex);
 // [cmdlock] The parent map used to stop accepting new views at 16384 entries. Entries leave only when their texture
 // is destroyed, so a long session filled it and every view created afterwards stayed parentless forever: each bind
 // group using such a view was refused by the bind-group cache and recreated on the main thread every frame
 // (~800k refusals vs 5.9k stores at 32 vehicles). Limit raised to 262144 (?viewParents=0 = old 16384).
 static const std::size_t parentLimit=MAIN_THREAD_EM_ASM_INT({return new URLSearchParams(location.search).get('viewParents')==='0'?0:1;})?262144:16384;
 if(!request.failed && request.view && (textureViewParents.size()<parentLimit || textureViewParents.count(request.view)))SetViewParentLocked(request.view,request.texture);
 else if(!request.failed && request.view){static bool warned=false;if(!warned){warned=true;std::fprintf(stderr,"CP2077 texture view parent map full (%u): new views are not bind-group cacheable\n",unsigned(textureViewParents.size()));}}
 if(request.failed||!request.view||request.key.empty()||request.device!=textureViewCacheDevice||textureViewCache.count(request.key))return;
 if(textureViewCache.size()>=textureViewEntryLimit){auto oldest=std::min_element(textureViewCache.begin(),textureViewCache.end(),[](const auto& a,const auto& b){return a.second.stamp<b.second.stamp;});EraseView(oldest,0);}
 wgpuTextureAddRef(request.texture);wgpuTextureViewAddRef(request.view);{std::unique_lock<std::shared_mutex> rw(textureViewRw);textureViewCache.emplace(request.key,CachedTextureView{request.texture,request.view,++textureViewStamp});cachedTextureViewHandles.insert(request.view);}cacheKeysByTexture[request.texture].push_back(request.key);PublishViews();
}
bool ReuseTextureViewOnWorker(ViewRequest& request){
 // perf11: ~65k hits/s across render workers; the key (heap string) is now built before taking the shared mutex,
 // into a per-thread buffer. request.key stays empty: a miss rebuilds it on the main thread (ReuseTextureView).
 if(!cp2077::web::IsGpuDeviceReady())return false;
 thread_local std::string key;BuildTextureViewKey(key,request);if(key.empty())return false;
 std::shared_lock<std::shared_mutex> guard(textureViewRw);
 if(!textureViewCacheDevice||textureViewGeneration!=cp2077::web::GpuDeviceGeneration())return false;
 auto it=textureViewCache.find(key);if(it==textureViewCache.end())return false;
 request.view=it->second.view;wgpuTextureViewAddRef(request.view);++textureViewHits;++textureViewWorkerHits;return true;
}
bool ReleaseCachedTextureViewOnWorker(WGPUTextureView view){
 std::shared_lock<std::shared_mutex> guard(textureViewRw);
 if(!cachedTextureViewHandles.count(view))return false;
 // Cache ownership keeps this decrement from destroying the GPU wrapper.
 wgpuTextureViewRelease(view);++textureViewWorkerReleases;return true;
}
