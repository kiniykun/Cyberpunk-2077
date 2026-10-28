namespace GpuApi {
void PrecachePSO(const PSOPrecacheDesc& original, job::Builder*) {
    using namespace cp2077::web;
    // CPU-side synchronous creation provides a completed dependency before
    // returning. Native GPU pipeline creation/validation is still asynchronous
    // on the main runtime thread and is awaited by CreateGpuRenderPipeline.
    ShaderRef acquired[2]{};
    OMStateRef heldOm=OMStateRef::Null();
    WGPUShaderModule modules[2]{};
    const ConvertedGraphicsShader* sources[2]{};
    std::vector<WGPURenderPipeline> created;
    auto cleanup=[&] {
        for (auto pipeline : created) ReleaseGpuRenderPipeline(pipeline);
        for (auto shader : acquired) if (!shader.isNull()) Release(shader);
        if (!heldOm.isNull()) Release(heldOm);
    };
    auto reject=[&] {
        cleanup(); cp2077::web::FailBoundGpuCommand();
        std::fputs("CP2077 WebGPU: original PSO precache has unsupported stages/state or pipeline validation failed\n",stderr);
    };
    if (!IsInit() || original.m_rtFormats.Size() > MAX_RENDERTARGETS ||
        (original.m_sampleCount != 1 && original.m_sampleCount != 4)) { reject(); return; }
    for (Uint32 stage=0;stage<GpuWrapApi::ShaderType_GraphicsMax;++stage)
        if (stage != GpuWrapApi::VertexShader && stage != GpuWrapApi::PixelShader && !original.m_shaderRefs[stage].isNull()) {
            reject(); return;
        }
    if (!AcquireGpuGraphicsShader(original.m_shaderRefs[GpuWrapApi::VertexShader],GraphicsShaderStage::Vertex,modules[0],sources[0])) {
        reject(); return;
    }
    acquired[0]=original.m_shaderRefs[GpuWrapApi::VertexShader];
    if (!original.m_shaderRefs[GpuWrapApi::PixelShader].isNull()) {
        if (!AcquireGpuGraphicsShader(original.m_shaderRefs[GpuWrapApi::PixelShader],GraphicsShaderStage::Pixel,modules[1],sources[1])) {
            reject(); return;
        }
        acquired[1]=original.m_shaderRefs[GpuWrapApi::PixelShader];
    } else if (!original.m_rtFormats.Empty()) { reject(); return; }
    SOMState nativeOm;
    if (!AcquireGpuOMState(original.m_omState,nativeOm)) { reject(); return; }
    heldOm=original.m_omState;
    GpuOMPipelineState om;
    const auto depthFormat=TranslateGpuTextureFormat(original.m_dsFormat);
    if ((original.m_dsFormat != GpuWrapApi::TEXFMT_NULL && depthFormat == WGPUTextureFormat_Undefined) ||
        !BuildGpuOMPipelineState(nativeOm,original.m_depthClipEnabled,original.m_rtFormats.Size(),depthFormat,original.m_sampleCount,om)) {
        reject(); return;
    }
    GpuVertexBufferLayouts geometry;
    if (!BuildGpuVertexBufferLayouts(original.m_vertexLayoutRef,*sources[0],0,nullptr,geometry)) { reject(); return; }
    WGPUColorTargetState targets[MAX_RENDERTARGETS];
    for (Uint32 i=0;i<original.m_rtFormats.Size();++i) {
        targets[i]=WGPU_COLOR_TARGET_STATE_INIT;
        targets[i].format=TranslateGpuTextureFormat(original.m_rtFormats[i]);
        if (targets[i].format == WGPUTextureFormat_Undefined) { reject(); return; }
        // [notier1] storage render targets of this format get another core format; the precache can't know the usage,
        // so leave these pipelines to the draw path (which targets the real attachment format).
        if (cp2077::web::GpuLacksTier1() && cp2077::web::Tier1CoreFormat(targets[i].format,
                WGPUTextureUsage_StorageBinding | WGPUTextureUsage_RenderAttachment) != targets[i].format) { reject(); return; }
        targets[i].blend=om.blendEnabled[i] ? &om.blends[i] : nullptr;
        const bool eyeShadowOnlyTarget0 = sources[1] && sources[1]->stage==GraphicsShaderStage::Pixel &&
            ((std::strcmp(sources[1]->originalHash,"12784076727638384549")==0 &&
              GraphicsWgslHashMatches(sources[1],"dfc09da81a26e060926d5788aea2e25f3117698acae47776519a08ac25fdd382")) ||
             (std::strcmp(sources[1]->originalHash,"17262612989000979035")==0 &&
              GraphicsWgslHashMatches(sources[1],"030b996f1082770f55ef1d463f5668e628b8ac693466a03d9c1ff224bcb7edf7")));
        targets[i].writeMask=(eyeShadowOnlyTarget0 && i>0) ? WGPUColorWriteMask_None : om.writeMasks[i];
    }
    WGPUFragmentState fragment=WGPU_FRAGMENT_STATE_INIT;
    fragment.module=modules[1]; fragment.entryPoint={"main",WGPU_STRLEN};
    if (sources[1] && FindConvertedGraphicsGiRGBOutputVariant(sources[1]) && original.m_rtFormats.Size()==1 &&
        targets[0].format==WGPUTextureFormat_RGBA16Float) {
        if(om.blendEnabled[0] || om.alphaToCoverage){reject();return;}
        fragment.module=GetGpuGraphicsGiRGBOutputModule(acquired[1]);
        if(!fragment.module){reject();return;}
        targets[0].writeMask=static_cast<WGPUColorWriteMask>(targets[0].writeMask &
            (WGPUColorWriteMask_Red|WGPUColorWriteMask_Green|WGPUColorWriteMask_Blue));
    }
    // Same output-location write-mask rule as the draw path (keeps precached keys identical to draw keys).
    if (modules[1] && fragment.module==modules[1]) {
        const std::uint32_t outputs=GraphicsFragmentOutputMask(sources[1]);
        for (Uint32 i=0;i<original.m_rtFormats.Size();++i)
            if (!(outputs & (1u<<i))) { targets[i].writeMask=WGPUColorWriteMask_None; targets[i].blend=nullptr; }
    }
    fragment.targetCount=original.m_rtFormats.Size(); fragment.targets=targets;
    WGPURenderPipelineDescriptor descriptor=WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    descriptor.vertex.module=modules[0]; descriptor.vertex.entryPoint={"main",WGPU_STRLEN};
    descriptor.vertex.bufferCount=geometry.buffers.size(); descriptor.vertex.buffers=geometry.buffers.data();
    descriptor.fragment=modules[1] ? &fragment : nullptr;
    descriptor.depthStencil=om.hasDepthStencil ? &om.depthStencil : nullptr;
    descriptor.multisample.count=original.m_sampleCount;
    descriptor.multisample.alphaToCoverageEnabled=om.alphaToCoverage;
    descriptor.primitive=om.primitive;
    std::vector<std::pair<WGPUPrimitiveTopology,WGPUIndexFormat>> variants;
    using Type=decltype(original.m_primitiveTopology);
    switch (original.m_primitiveTopology) {
    case Type::Point: variants.push_back({WGPUPrimitiveTopology_PointList,WGPUIndexFormat_Undefined}); break;
    case Type::Line:
        variants.push_back({WGPUPrimitiveTopology_LineList,WGPUIndexFormat_Undefined});
        for (auto index : {WGPUIndexFormat_Undefined,WGPUIndexFormat_Uint16,WGPUIndexFormat_Uint32})
            variants.push_back({WGPUPrimitiveTopology_LineStrip,index});
        break;
    case Type::Triangle:
        variants.push_back({WGPUPrimitiveTopology_TriangleList,WGPUIndexFormat_Undefined});
        for (auto index : {WGPUIndexFormat_Undefined,WGPUIndexFormat_Uint16,WGPUIndexFormat_Uint32})
            variants.push_back({WGPUPrimitiveTopology_TriangleStrip,index});
        break;
    default: reject(); return;
    }
    // Match the draw path's explicit (dynamic-offset) layout so precached pipelines hit the cache at draw time.
    GpuGraphicsPipelineLayout explicitLayout;
    bool useExplicit=fragment.module==modules[1] && AcquireGpuGraphicsPipelineLayout(sources[0],modules[1]?sources[1]:nullptr,explicitLayout);
    descriptor.layout=useExplicit?explicitLayout.layout:nullptr;
    if (useExplicit) { // [nomadperf] ovfStorage converted modules
        if (explicitLayout.vertexModule) descriptor.vertex.module=explicitLayout.vertexModule;
        if (descriptor.fragment && explicitLayout.fragmentModule) fragment.module=explicitLayout.fragmentModule;
    }
    for (auto variant : variants) {
        descriptor.primitive.topology=variant.first; descriptor.primitive.stripIndexFormat=variant.second;
        // [hitch] Fire-and-forget: a miss starts creation and returns pending (the cache keeps the pipeline),
        // so the precache job no longer waits for every variant to compile. ?asyncPso=0 = old blocking path.
        bool pending=false;
        auto pipeline=CreateGpuRenderPipelineOrPending(descriptor,pending,true);
        if (pending) continue;
        if (!pipeline && useExplicit) {
            useExplicit=false; descriptor.layout=nullptr; descriptor.vertex.module=modules[0]; if (descriptor.fragment) fragment.module=modules[1];
            pipeline=CreateGpuRenderPipelineOrPending(descriptor,pending,true);
            if (pending) continue;
            if (pipeline) RejectGpuGraphicsPipelineLayout(sources[0],modules[1]?sources[1]:nullptr);
        }
        if (!pipeline) { reject(); return; }
        created.push_back(pipeline);
    }
    {
        std::lock_guard<std::mutex> lock(precacheMutex);
        precachedPipelines.insert(precachedPipelines.end(),created.begin(),created.end()); created.clear();
    }
    cleanup();
}
}
