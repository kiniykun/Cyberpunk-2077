// Included in cp2077::web. Original R16_TYPELESS depth->UNORM copies keep
// exactly the same 16-bit codes; WebGPU requires separate typed resources.
static constexpr char kDepthUnorm16CopyWGSL[]=R"WGSL(
struct CopyRegion { source:vec2<u32>, destination:vec2<u32>, extent:vec2<u32>, pad:vec2<u32> }
@group(0) @binding(0) var source:texture_depth_2d;
@group(0) @binding(1) var destination:texture_storage_2d<r16unorm,write>;
@group(0) @binding(2) var<uniform> region:CopyRegion;
@compute @workgroup_size(8,8)
fn copyDepth16(@builtin(global_invocation_id) id:vec3<u32>) {
    if(any(id.xy>=region.extent)){return;}
    let value=textureLoad(source,vec2<i32>(region.source+id.xy),0);
    textureStore(destination,vec2<i32>(region.destination+id.xy),vec4<f32>(value,0,0,1));
}
)WGSL";
// Main thread only. One module+pipeline per device generation (was compiled on every copy, perf7).
static WGPUComputePipeline DepthUnorm16CopyPipelineOnMain(WGPUDevice device) {
    static WGPUComputePipeline cached=nullptr;static WGPUDevice cachedDevice=nullptr;static std::uint64_t cachedGeneration=0;
    const auto generation=GpuDeviceGeneration();
    if(cached&&cachedDevice==device&&cachedGeneration==generation)return cached;
    if(cached){wgpuComputePipelineRelease(cached);cached=nullptr;}
    WGPUShaderSourceWGSL wgsl=WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code={kDepthUnorm16CopyWGSL,sizeof(kDepthUnorm16CopyWGSL)-1};
    WGPUShaderModuleDescriptor moduleDesc=WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    moduleDesc.nextInChain=&wgsl.chain;
    auto module=wgpuDeviceCreateShaderModule(device,&moduleDesc);if(!module)return nullptr;
    WGPUComputePipelineDescriptor desc=WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    desc.compute.module=module;desc.compute.entryPoint={"copyDepth16",WGPU_STRLEN};
    cached=wgpuDeviceCreateComputePipeline(device,&desc);wgpuShaderModuleRelease(module);
    cachedDevice=device;cachedGeneration=generation;
    return cached;
}
static bool EncodeDepthUnorm16CopyOnMain(WGPUCommandEncoder encoder,WGPUTextureView source,WGPUTextureView destination,
    WGPUBuffer region,std::uint64_t regionOffset,std::uint32_t width,std::uint32_t height) {
    auto device=MainThreadGpuDevice();if(!device)return false;
    auto pipeline=DepthUnorm16CopyPipelineOnMain(device);if(!pipeline)return false;
    auto layout=wgpuComputePipelineGetBindGroupLayout(pipeline,0);
    if(!layout)return false;
    WGPUBindGroupEntry entries[3];for(auto& e:entries)e=WGPU_BIND_GROUP_ENTRY_INIT;
    entries[0].binding=0;entries[0].textureView=source;
    entries[1].binding=1;entries[1].textureView=destination;
    entries[2].binding=2;entries[2].buffer=region;entries[2].offset=regionOffset;entries[2].size=32;
    WGPUBindGroupDescriptor groupDesc=WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    groupDesc.layout=layout;groupDesc.entryCount=3;groupDesc.entries=entries;
    auto group=wgpuDeviceCreateBindGroup(device,&groupDesc);wgpuBindGroupLayoutRelease(layout);
    if(!group)return false;
    bool issued=false;
    WGPUComputePassDescriptor passDesc=WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
    auto pass=wgpuCommandEncoderBeginComputePass(encoder,&passDesc);
    if(pass){
        wgpuComputePassEncoderSetPipeline(pass,pipeline);
        wgpuComputePassEncoderSetBindGroup(pass,0,group,0,nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(pass,(width+7)/8,(height+7)/8,1);
        wgpuComputePassEncoderEnd(pass);wgpuComputePassEncoderRelease(pass);issued=true;
    }
    wgpuBindGroupRelease(group);
    return issued;
}
bool EncodeGpuDepthUnorm16CopyAt(WGPUCommandEncoder encoder,WGPUTextureView source,
    WGPUTextureView destination,WGPUBuffer region,std::uint64_t regionOffset,std::uint32_t width,std::uint32_t height) {
    if(!encoder||!source||!destination||!region||regionOffset%256||!width||!height)return false;
    struct Request {WGPUCommandEncoder encoder;WGPUTextureView source,destination;WGPUBuffer region;std::uint64_t offset;
        std::uint32_t width,height;bool issued=false;} request{encoder,source,destination,region,regionOffset,width,height};
    return OnMain([](void* argument){
        auto& r=*static_cast<Request*>(argument);
        r.issued=EncodeDepthUnorm16CopyOnMain(r.encoder,r.source,r.destination,r.region,r.offset,r.width,r.height);
    },&request)&&request.issued;
}
bool EncodeGpuDepthUnorm16Copy(WGPUCommandEncoder encoder,WGPUTextureView source,
    WGPUTextureView destination,WGPUBuffer region,std::uint32_t width,std::uint32_t height) {
    return EncodeGpuDepthUnorm16CopyAt(encoder,source,destination,region,0,width,height);
}
