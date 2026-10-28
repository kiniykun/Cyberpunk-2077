// Included in cp2077::web. These kernels lower integer XOR blending; original
// geometry, shader calculation, coverage, depth and stencil stay in the draw.
static const char kXorInitializeWGSL[]=R"XORWGSL(
@group(0) @binding(0) var source:texture_2d<u32>;
@group(0) @binding(1) var bits:texture_storage_2d_array<r8unorm,write>;
@compute @workgroup_size(8,8) fn main(@builtin(global_invocation_id) p:vec3u){
 let size=textureDimensions(source);if(any(p.xy>=size)){return;}
 let value=textureLoad(source,vec2i(p.xy),0).x;
 for(var bit=0u;bit<8u;bit++){textureStore(bits,vec2i(p.xy),i32(bit),vec4f(f32((value>>bit)&1u)));}
})XORWGSL";
static const char kXorResolveWGSL[]=R"XORWGSL(
@group(0) @binding(0) var bits:texture_2d_array<f32>;
@vertex fn vertex(@builtin(vertex_index) i:u32)->@builtin(position) vec4f{
 let p=array<vec2f,3>(vec2f(-1,-1),vec2f(3,-1),vec2f(-1,3));return vec4f(p[i],0,1);
}
@fragment fn pixel(@builtin(position) p:vec4f)->@location(0) u32{
 var value=0u;for(var bit=0u;bit<8u;bit++){value|=u32(textureLoad(bits,vec2i(p.xy),i32(bit),0).x)<<bit;}return value;
})XORWGSL";
// perf8: the initialize/resolve pipelines are created once per device generation and shared by every scratch
// (they used to be compiled from WGSL for every command list, each costing synchronous browser round trips).
// A previous generation's pair is not released: lists recorded on that device may still reference it.
namespace {
std::mutex xorSharedMutex;
struct XorSharedPipelines { std::uint64_t generation=0; WGPUComputePipeline initialize=nullptr; WGPURenderPipeline resolve=nullptr; } xorShared;
std::vector<GpuLogicXorScratch> xorPool; // idle scratches, guarded by xorSharedMutex (oldest first)
constexpr std::size_t kXorPoolLimit=8;
// [skperf] Command lists return their scratches only when the GPU retires the submission, so with async present
// several frames of XOR targets are outstanding at once. Eight idle slots overflowed: Street Kid's bar recycled
// into a full pool and destroyed the scratch (texture + 10 views + bind groups, a worker->main destroy that drains
// the deferred submit backlog), then created it again next frame (~24 creations/s, 2048 by +140 s). The pool now
// keeps up to 128 idle scratches / 128 MiB and evicts the oldest idle one instead of refusing the newest.
// ?xorPool=0 = old (8 idle slots, refuse when full).
constexpr std::size_t kXorBigPoolLimit=128;
constexpr std::uint64_t kXorBigPoolBytes=128ull<<20;
std::uint64_t xorPoolBytes=0; // guarded by xorSharedMutex
std::atomic<unsigned> xorPoolHits{0},xorPoolEvictions{0};
std::atomic<int> xorBigPoolState{-1};
bool XorBigPool(){
 int v=xorBigPoolState.load(std::memory_order_acquire);
 if(v<0){
  v=MAIN_THREAD_EM_ASM_INT({return new URLSearchParams(location.search).get('xorPool')==='0'?0:1;});
  xorBigPoolState.store(v,std::memory_order_release);
  std::fprintf(stderr,"CP2077 skperf switches: xorPool=%d\n",v);
 }
 return v!=0;
}
std::uint64_t XorScratchBytes(const GpuLogicXorScratch& s){return std::uint64_t(s.width)*s.height*8;}
// The main thread never blocks on xorSharedMutex: a worker holds it across synchronous main-thread handoffs.
bool LockXorShared(std::unique_lock<std::mutex>& lock){
 if(emscripten_is_main_runtime_thread())return lock.try_lock();
 lock.lock();return true;
}
}
static bool AcquireXorPipelinesLocked(std::uint64_t generation,WGPUComputePipeline& initialize,WGPURenderPipeline& resolve){
 if(!(xorShared.generation==generation && xorShared.initialize && xorShared.resolve)){
  xorShared={};
  static const ConvertedShader source=[](){ConvertedShader v{};v.name=v.runtimeName="cpweb-xor-initialize";v.entryPoint="main";v.wgsl=kXorInitializeWGSL;v.wgslBytes=sizeof(kXorInitializeWGSL)-1;v.requiresTier1Storage=true;return v;}();
  auto compute=CreateGpuComputePipelineFromSource(&source);if(!compute)return false;
  ConvertedGraphicsShader shader{};shader.originalHash="cpweb-xor-resolve";shader.wgsl=kXorResolveWGSL;shader.wgslBytes=sizeof(kXorResolveWGSL)-1;
  auto module=CreateConvertedGraphicsModule(&shader);if(!module){ReleaseGpuComputePipeline(compute);return false;}
  WGPUColorTargetState target=WGPU_COLOR_TARGET_STATE_INIT;target.format=WGPUTextureFormat_R8Uint;
  WGPUFragmentState fragment=WGPU_FRAGMENT_STATE_INIT;fragment.module=module;fragment.entryPoint={"pixel",WGPU_STRLEN};fragment.targetCount=1;fragment.targets=&target;
  WGPURenderPipelineDescriptor pd=WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;pd.vertex.module=module;pd.vertex.entryPoint={"vertex",WGPU_STRLEN};pd.fragment=&fragment;
  auto render=CreateGpuRenderPipeline(pd);ReleaseGpuGraphicsModule(module);if(!render){ReleaseGpuComputePipeline(compute);return false;}
  xorShared={generation,compute,render};
 }
 initialize=xorShared.initialize;resolve=xorShared.resolve;return true;
}
void ReleaseGpuLogicXorScratch(GpuLogicXorScratch& s){
 // Releases only this scratch's own objects; initialize/resolve are the shared per-generation pipelines.
 if(s.resolveGroup)ReleaseGpuBindGroup(s.resolveGroup);
 for(auto view:s.layers)if(view)ReleaseGpuTextureView(view);
 if(s.storage)ReleaseGpuTextureView(s.storage);
 if(s.sampled)ReleaseGpuTextureView(s.sampled);
 if(s.texture)DestroyGpuTexture(s.texture);
 s={};
}
bool CreateGpuLogicXorScratch(std::uint32_t width,std::uint32_t height,GpuLogicXorScratch& s){
 if(s.texture || !width || !height || std::uint64_t(width)*height*8>64ull*1024*1024)return false;
 const std::uint64_t generation=GpuDeviceGeneration();
 WGPUComputePipeline initialize=nullptr;WGPURenderPipeline resolve=nullptr;
 {std::unique_lock<std::mutex> lock(xorSharedMutex,std::defer_lock);if(!LockXorShared(lock) || !AcquireXorPipelinesLocked(generation,initialize,resolve))return false;}
 s.width=width;s.height=height;
 WGPUTextureDescriptor td=WGPU_TEXTURE_DESCRIPTOR_INIT;td.dimension=WGPUTextureDimension_2D;
 td.size={width,height,8};td.mipLevelCount=1;td.sampleCount=1;td.format=WGPUTextureFormat_R8Unorm;
 td.usage=WGPUTextureUsage_RenderAttachment|WGPUTextureUsage_TextureBinding|WGPUTextureUsage_StorageBinding;
 s.texture=CreateGpuTexture(td);if(!s.texture)return false;
 auto fail=[&](){ReleaseGpuLogicXorScratch(s);return false;};
 WGPUTextureViewDescriptor vd=WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;vd.format=td.format;
 vd.dimension=WGPUTextureViewDimension_2DArray;vd.mipLevelCount=1;vd.arrayLayerCount=8;
 vd.usage=WGPUTextureUsage_StorageBinding;s.storage=CreateGpuTextureView(s.texture,vd);
 vd.usage=WGPUTextureUsage_TextureBinding;s.sampled=CreateGpuTextureView(s.texture,vd);
 vd.dimension=WGPUTextureViewDimension_2D;vd.arrayLayerCount=1;vd.usage=WGPUTextureUsage_RenderAttachment;
 for(unsigned i=0;i<8;++i){vd.baseArrayLayer=i;s.layers[i]=CreateGpuTextureView(s.texture,vd);if(!s.layers[i])return fail();}
 if(!s.storage||!s.sampled)return fail();
 s.initialize=initialize;s.resolve=resolve;s.generation=generation;
 WGPUBindGroupEntry entry=WGPU_BIND_GROUP_ENTRY_INIT;entry.binding=0;entry.textureView=s.sampled;
 s.resolveGroup=CreateGpuGraphicsBindGroup(s.resolve,0,&entry,1);if(!s.resolveGroup)return fail();
 return true;
}
bool AcquireGpuLogicXorScratch(std::uint32_t width,std::uint32_t height,GpuLogicXorScratch& s){
 // Every XOR draw's initialize dispatch rewrites all 8 layers over the full extent before the draw reads them,
 // so a recycled scratch's earlier contents never reach a result; queue order serializes reuse on the GPU.
 if(s.texture)return false;
 const std::uint64_t generation=GpuDeviceGeneration();
 std::vector<GpuLogicXorScratch> stale;bool found=false;
 {
  std::unique_lock<std::mutex> lock(xorSharedMutex,std::defer_lock);
  if(LockXorShared(lock))for(auto it=xorPool.begin();it!=xorPool.end();){
   if(it->generation!=generation){xorPoolBytes-=XorScratchBytes(*it);stale.push_back(*it);it=xorPool.erase(it);continue;}
   if(!found && it->width==width && it->height==height){s=*it;xorPoolBytes-=XorScratchBytes(*it);it=xorPool.erase(it);found=true;continue;}
   ++it;
  }
 }
 for(auto& old:stale)ReleaseGpuLogicXorScratch(old);
 if(found){xorPoolHits.fetch_add(1,std::memory_order_relaxed);return true;}
 static std::atomic<unsigned> created{0};
 const unsigned n=created.fetch_add(1)+1;
 if(n<=4 || !(n&(n-1)))std::fprintf(stderr,"CP2077 XOR scratch pool create: count=%u width=%u height=%u hits=%u evictions=%u bigPool=%d\n",
  n,width,height,xorPoolHits.load(),xorPoolEvictions.load(),xorBigPoolState.load());
 return CreateGpuLogicXorScratch(width,height,s);
}
void RecycleGpuLogicXorScratch(GpuLogicXorScratch& s){
 if(!s.texture){s={};return;}
 bool kept=false;
 const bool big=XorBigPool(); // read before taking the pool lock (may proxy to the main thread once)
 std::vector<GpuLogicXorScratch> evicted;
 if(s.generation==GpuDeviceGeneration()){
  std::unique_lock<std::mutex> lock(xorSharedMutex,std::defer_lock);
  if(LockXorShared(lock)){
   if(!big){if(xorPool.size()<kXorPoolLimit){xorPool.push_back(s);xorPoolBytes+=XorScratchBytes(s);kept=true;}}
   else if(XorScratchBytes(s)<=kXorBigPoolBytes){
    xorPool.push_back(s);xorPoolBytes+=XorScratchBytes(s);kept=true;
    while(xorPool.size()>1 && (xorPool.size()>kXorBigPoolLimit || xorPoolBytes>kXorBigPoolBytes)){
     xorPoolBytes-=XorScratchBytes(xorPool.front());evicted.push_back(xorPool.front());xorPool.erase(xorPool.begin());
    }
   }
  }
 }
 if(!kept)ReleaseGpuLogicXorScratch(s);
 if(!evicted.empty())xorPoolEvictions.fetch_add(unsigned(evicted.size()),std::memory_order_relaxed);
 for(auto& old:evicted)ReleaseGpuLogicXorScratch(old);
 s={};
}
bool EncodeGpuLogicXorDraw(WGPUCommandEncoder encoder,const GraphicsDraw& draw,WGPUTextureView destination,
 WGPUTextureView destinationSampled,GpuLogicXorScratch& s,WGPUBindGroup& retainedInitializeGroup){
 retainedInitializeGroup=nullptr;
 if(!encoder||!destination||!destinationSampled||draw.colorCount!=8||draw.targetWidth!=s.width||draw.targetHeight!=s.height)return false;
 WGPUBindGroupEntry entries[2]={WGPU_BIND_GROUP_ENTRY_INIT,WGPU_BIND_GROUP_ENTRY_INIT};
 entries[0].binding=0;entries[0].textureView=destinationSampled;entries[1].binding=1;entries[1].textureView=s.storage;
 auto group=CreateGpuComputeBindGroup(s.initialize,0,entries,2);if(!group)return false;
 ComputeBindGroup initGroup{0,group};
 bool ok=EncodeGpuComputeDispatch(encoder,s.initialize,&initGroup,1,(s.width+7)/8,(s.height+7)/8,1);
 // Even a partially encoded operation retains this binding until submission
 // or cancellation; the caller owns it on either outcome.
 retainedInitializeGroup=group;
 if(!ok||!EncodeGpuGraphicsDraw(encoder,draw))return false;
 GraphicsDraw resolved;resolved.pipeline=s.resolve;resolved.colors=&destination;resolved.colorCount=1;
 resolved.targetWidth=s.width;resolved.targetHeight=s.height;resolved.viewportWidth=float(s.width);resolved.viewportHeight=float(s.height);
 resolved.scissorWidth=s.width;resolved.scissorHeight=s.height;resolved.count=3;resolved.instances=1;
 GraphicsDrawGroup binding{0,s.resolveGroup};resolved.groups=&binding;resolved.groupCount=1;
 return EncodeGpuGraphicsDraw(encoder,resolved);
}
