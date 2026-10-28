// Creation, destruction and validation stay on the main GPU thread. Exact
// immutable cache hits may retain a lease on a worker under the cache mutex.
// Busy evictions survive until retirement; no new buffers bypass validation
// (perf9: small buffers are validated asynchronously; a failure evicts the record, see AsyncAllocationChecked).
std::recursive_mutex immutableUniformMutex;
struct ImmutableUniformRecord {std::string key;WGPUBuffer buffer=nullptr;std::uint64_t bytes=0,stamp=0,leases=0;bool cached=false;std::list<WGPUBuffer>::iterator lru;};
std::map<std::string,WGPUBuffer> immutableUniformKeys;
std::map<WGPUBuffer,ImmutableUniformRecord> immutableUniformRecords;
std::list<WGPUBuffer> immutableUniformLru;
WGPUDevice immutableUniformDevice=nullptr;
std::uint64_t immutableUniformBytes=0,immutableUniformStamp=0,immutableUniformMisses=0;
std::uint64_t immutableUniformGeneration=0;
std::atomic<std::uint64_t> immutableUniformHits{0},immutableUniformWorkerHits{0},immutableUniformWorkerReleases{0};
constexpr std::uint64_t immutableUniformBudget=8ull*1024*1024;
constexpr std::size_t immutableUniformEntryBudget=8192;
EM_JS(void,PublishImmutableUniformStats,(double hits,double misses,double bytes,int cached,int live,double workerHits,void* totalCounter,void* workerCounter,void* releaseCounter),{
 const stats=Module['cp2077ImmutableUniformStats']={misses,cachedBytes:bytes,cachedEntries:cached,liveRecords:live};
 for(const [name,address] of [['hits',Number(totalCounter)],['workerHits',Number(workerCounter)],['workerReleases',Number(releaseCounter)]])
  Object.defineProperty(stats,name,{enumerable:true,get(){growMemViews();return Number(Atomics.load(HEAPU64,address/8));}});
});
void PublishUniforms(){std::lock_guard<std::recursive_mutex> guard(immutableUniformMutex);PublishImmutableUniformStats(double(immutableUniformHits.load()),double(immutableUniformMisses),double(immutableUniformBytes),int(immutableUniformKeys.size()),int(immutableUniformRecords.size()),double(immutableUniformWorkerHits.load()),&immutableUniformHits,&immutableUniformWorkerHits,&immutableUniformWorkerReleases);}
void TouchUniform(ImmutableUniformRecord& record){std::lock_guard<std::recursive_mutex> guard(immutableUniformMutex);record.stamp=++immutableUniformStamp;if(record.cached)immutableUniformLru.splice(immutableUniformLru.end(),immutableUniformLru,record.lru);}
void DestroyUniformRecord(std::map<WGPUBuffer,ImmutableUniformRecord>::iterator it){
 std::lock_guard<std::recursive_mutex> guard(immutableUniformMutex);
 cp2077::web::InvalidateGpuBindGroupsForBuffer(it->first);wgpuBufferDestroy(it->first);immutableUniformRecords.erase(it);
}
void EvictUniform(WGPUBuffer buffer){
 std::lock_guard<std::recursive_mutex> guard(immutableUniformMutex);
 auto it=immutableUniformRecords.find(buffer);if(it==immutableUniformRecords.end()||!it->second.cached)return;
 immutableUniformLru.erase(it->second.lru);
 immutableUniformKeys.erase(it->second.key);immutableUniformBytes-=it->second.bytes;it->second.cached=false;std::string().swap(it->second.key);
 if(!it->second.leases)DestroyUniformRecord(it);wgpuBufferRelease(buffer); // Drop only cache ownership.
}
void ClearUniforms(){std::lock_guard<std::recursive_mutex> guard(immutableUniformMutex);while(!immutableUniformKeys.empty())EvictUniform(immutableUniformKeys.begin()->second);if(immutableUniformDevice)wgpuDeviceRelease(immutableUniformDevice);immutableUniformDevice=nullptr;immutableUniformGeneration=0;PublishUniforms();}
bool ReleaseUniformLease(WGPUBuffer buffer){
 std::lock_guard<std::recursive_mutex> guard(immutableUniformMutex);
 auto it=immutableUniformRecords.find(buffer);if(it==immutableUniformRecords.end())return false;
 if(!it->second.leases){std::fputs("CP2077 immutable uniform: unmatched release\n",stderr);return true;}
 --it->second.leases;if(!it->second.cached&&!it->second.leases)DestroyUniformRecord(it);
 wgpuBufferRelease(buffer);PublishUniforms();return true;
}
bool ReleaseCachedUniformLeaseOnWorker(WGPUBuffer buffer){
 std::lock_guard<std::recursive_mutex> guard(immutableUniformMutex);
 auto it=immutableUniformRecords.find(buffer);
 if(it==immutableUniformRecords.end()||!it->second.cached||!it->second.leases)return false;
 // Cache ownership guarantees this decrement cannot destroy the GPU wrapper.
 // Uncached/final leases still perform invalidation and destruction on main.
 --it->second.leases;wgpuBufferRelease(buffer);++immutableUniformWorkerReleases;return true;
}
bool ReuseUniform(Allocation& request){
 std::lock_guard<std::recursive_mutex> guard(immutableUniformMutex);
 if(!request.immutable)return false;
 if(immutableUniformDevice!=request.device || immutableUniformGeneration!=cp2077::web::GpuDeviceGeneration()){ClearUniforms();immutableUniformDevice=request.device;immutableUniformGeneration=cp2077::web::GpuDeviceGeneration();wgpuDeviceAddRef(request.device);}
 if(request.immutableKey.empty()){++immutableUniformMisses;return false;}
 auto key=immutableUniformKeys.find(request.immutableKey);
 if(key==immutableUniformKeys.end()){++immutableUniformMisses;return false;}
 auto& record=immutableUniformRecords.at(key->second);++record.leases;TouchUniform(record);++immutableUniformHits;
 request.buffer=record.buffer;wgpuBufferAddRef(record.buffer);PublishUniforms();request.cacheHit=true;request.completedAt=emscripten_get_now();request.trace.CallEnd();emscripten_proxy_finish(request.context);return true;
}
void StoreUniform(Allocation& request){
 std::lock_guard<std::recursive_mutex> guard(immutableUniformMutex);
 if(!request.immutable||request.failed||!request.buffer||request.device!=immutableUniformDevice)return;
 auto existing=immutableUniformKeys.find(request.immutableKey);
 if(existing!=immutableUniformKeys.end()){
  auto& record=immutableUniformRecords.at(existing->second);wgpuBufferDestroy(request.buffer);wgpuBufferRelease(request.buffer);
  request.buffer=record.buffer;++record.leases;TouchUniform(record);wgpuBufferAddRef(record.buffer);++immutableUniformHits;PublishUniforms();return;
 }
 if(request.size>immutableUniformBudget){immutableUniformRecords.emplace(request.buffer,ImmutableUniformRecord{{},request.buffer,request.size,++immutableUniformStamp,1,false});PublishUniforms();return;}
 while(immutableUniformKeys.size()>=immutableUniformEntryBudget || immutableUniformBytes+request.size>immutableUniformBudget){
  if(immutableUniformLru.empty())return;EvictUniform(immutableUniformLru.front());
 }
 wgpuBufferAddRef(request.buffer);ImmutableUniformRecord record{request.immutableKey,request.buffer,request.size,++immutableUniformStamp,1,true};
 immutableUniformLru.push_back(request.buffer);record.lru=std::prev(immutableUniformLru.end());
 immutableUniformKeys.emplace(record.key,record.buffer);immutableUniformRecords.emplace(record.buffer,std::move(record));immutableUniformBytes+=request.size;PublishUniforms();
}
bool IsImmutableUniformRecord(WGPUBuffer buffer){
 std::lock_guard<std::recursive_mutex> guard(immutableUniformMutex);
 return immutableUniformRecords.count(buffer)!=0;
}
bool ReuseUniformOnWorker(Allocation& request){
 if(!request.immutable || request.immutableKey.empty() || emscripten_is_main_runtime_thread())return false;
 std::lock_guard<std::recursive_mutex> guard(immutableUniformMutex);
 if(!cp2077::web::IsGpuDeviceReady() || !immutableUniformDevice ||
    immutableUniformGeneration!=cp2077::web::GpuDeviceGeneration())return false;
 auto key=immutableUniformKeys.find(request.immutableKey);
 if(key==immutableUniformKeys.end())return false;
 auto found=immutableUniformRecords.find(key->second);
 if(found==immutableUniformRecords.end() || !found->second.cached || found->second.bytes!=request.size)return false;
 auto& record=found->second;
 // Emdawn's buffer AddRef changes atomic native reference counts only. The
 // cache's ownership is protected here; destruction remains on its owner.
 wgpuBufferAddRef(record.buffer);++record.leases;TouchUniform(record);
 ++immutableUniformHits;++immutableUniformWorkerHits;
 request.buffer=record.buffer;request.cacheHit=true;request.workerCacheHit=true;
 request.beganAt=request.queuedAt;
 request.encodedAt=request.completedAt=emscripten_get_now();
 return true;
}
