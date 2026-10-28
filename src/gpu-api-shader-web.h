#pragma once
#include <webgpu/webgpu.h>
namespace GpuApi { struct ShaderRef; }
namespace cp2077::web {
struct ConvertedShader;
struct ConvertedGraphicsShader;
enum class GraphicsShaderStage;
void ShutdownGpuShaders();
// Borrowed module; caller must own the original retained ShaderRef.
WGPUShaderModule GetGpuGraphicsDepthGatherModule(const GpuApi::ShaderRef&);
WGPUShaderModule GetGpuGraphicsGiRGBOutputModule(const GpuApi::ShaderRef&);
WGPUShaderModule GetGpuGraphicsLogicXorModule(const GpuApi::ShaderRef&);
// Acquires one engine reference; the command owner must release or transfer it.
bool AcquireGpuComputeShader(const GpuApi::ShaderRef&, WGPUComputePipeline&);
// Retains the original ShaderRef; module/source remain borrowed under that ref.
bool AcquireGpuGraphicsShader(const GpuApi::ShaderRef&, GraphicsShaderStage,
                             WGPUShaderModule&, const ConvertedGraphicsShader*&);
// Caller must already own a retained original ShaderRef.
const ConvertedShader* GetGpuComputeShaderSource(const GpuApi::ShaderRef&);
}
