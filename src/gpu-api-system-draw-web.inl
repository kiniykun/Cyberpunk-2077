// Original CPU fill callbacks feed real GPU buffers and the original draw path.
namespace {
bool CanIssueSystemDraw() {
    std::lock_guard<std::mutex> lock(commandMutex);
    auto entry = commands.find(bound.ID());
    if (entry == commands.end() || entry->second.owner != std::this_thread::get_id()) return false;
    if (entry->second.type != CommandListType::Default) MarkBrowserCommandFailed(entry->second, __func__, __LINE__);
    return !entry->second.failed;
}
// Fills a system-draw payload into the bound command's pooled block of this category (see SystemDrawArena).
// Returns the block's engine reference, which the command owns (callers must not release it), and the byte
// offset of the payload. Bytes reach the GPU with one queue write per block just before the command is submitted.
bool StageSystemDrawBuffer(Uint32 count, Uint32 stride, GpuWrapApi::eBufferChunkCategory category,
                           const FillBufferFunc& fill, BufferRef& reference, Uint32& offset) {
    const std::uint64_t bytes = std::uint64_t(count) * stride;
    if (!fill || !count || !stride || bytes > 0x7fffffffu) return false;
    // Match the native callback's aligned upload pointer and byte-size argument.
    std::vector<std::max_align_t> storage((bytes + sizeof(std::max_align_t)-1)/sizeof(std::max_align_t));
    fill(storage.data(), static_cast<Uint32>(bytes));
    const std::uint32_t aligned = static_cast<std::uint32_t>((bytes + 15) & ~std::uint64_t(15));
    for (int attempt = 0; attempt < 2; ++attempt) {
        CommandEntry::SystemDrawBlock fresh;
        // A new block is created outside commandMutex: creation proxies to the main thread and takes bufferMutex.
        if (attempt && !SystemDrawArena::Acquire(category, aligned, fresh)) return false;
        bool staged = false, rejected = false;
        {
            std::lock_guard<std::mutex> lock(commandMutex);
            auto entry = commands.find(bound.ID());
            if (entry == commands.end() || entry->second.owner != std::this_thread::get_id() || entry->second.failed) rejected = true;
            else {
                auto& blocks = entry->second.systemDrawBlocks;
                if (attempt) blocks.push_back(std::move(fresh));
                for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
                    if (it->category != category || it->bytes.size() + aligned > it->capacity) continue;
                    offset = static_cast<Uint32>(it->bytes.size());
                    it->bytes.resize(it->bytes.size() + aligned, 0);
                    std::memcpy(it->bytes.data() + offset, storage.data(), static_cast<std::size_t>(bytes));
                    reference = it->reference; staged = true; break;
                }
            }
        }
        if (rejected) { if (attempt) SystemDrawArena::Release(fresh); return false; }
        if (staged) return true;
    }
    return false;
}
void IssueSystemIndexedDraw(bool index32, ePrimitiveType primitive, Uint32 vertexCount, Uint32 indexCount,
                            VertexLayoutRef layout, const FillBufferFunc& vertexFill, const FillBufferFunc& indexFill,
                            Uint32 instances, Uint32 instanceOffset) {
    if (!vertexCount || !indexCount) return;
    if (!CanIssueSystemDraw()) return;
    const Uint8 stride = GetVertexLayoutStride(layout);
    if (!GetVertexLayoutDesc(layout) || !stride || stride % 4 || !vertexFill || !indexFill) {
        cp2077::web::FailBoundGpuCommand(); return;
    }
    BufferRef vertex = BufferRef::Null(), index = BufferRef::Null();
    Uint32 offset = 0, indexOffset = 0;
    if (!StageSystemDrawBuffer(vertexCount, stride, GpuWrapApi::BCC_Vertex, vertexFill, vertex, offset)) {
        cp2077::web::FailBoundGpuCommand(); return;
    }
    if (!StageSystemDrawBuffer(indexCount, index32 ? 4 : 2,
            index32 ? GpuWrapApi::BCC_Index32Bit : GpuWrapApi::BCC_Index16Bit, indexFill, index, indexOffset)) {
        cp2077::web::FailBoundGpuCommand(); return;
    }
    // Bindings and issued draws retain their own engine refs; the pooled blocks stay owned by the command.
    BindVertexAndIndexBuffers(layout, 1, &vertex, &stride, &offset, index, indexOffset);
    DrawIndexedPrimitive(primitive, 0, 0, indexCount, instances, instanceOffset);
}
}
void DrawSystemPrimitive(ePrimitiveType primitive, Uint32 vertexCount, VertexLayoutRef layout,
                         const FillBufferFunc& fill, Uint32 instances, Uint32 instanceOffset, ComputeCuUsage) {
    if (!vertexCount) return;
    if (!CanIssueSystemDraw()) return;
    const Uint8 stride = GetVertexLayoutStride(layout);
    if (!GetVertexLayoutDesc(layout) || !stride || stride % 4 || !fill) {
        cp2077::web::FailBoundGpuCommand(); return;
    }
    BufferRef vertex = BufferRef::Null();
    Uint32 offset = 0;
    if (!StageSystemDrawBuffer(vertexCount, stride, GpuWrapApi::BCC_Vertex, fill, vertex, offset)) {
        cp2077::web::FailBoundGpuCommand(); return;
    }
    SetVertexFormatRaw(layout);
    BindVertexBuffers(0, 1, &vertex, &stride, &offset);
    DrawPrimitive(primitive, 0, vertexCount, instances, instanceOffset);
}
void DrawSystemIndexedPrimitive(ePrimitiveType primitive, Uint32 vertexCount, Uint32 indexCount,
                                VertexLayoutRef layout, const FillBufferFunc& vertexFill,
                                const FillIndexBuffer16Func& indexFill, Uint32 instances, Uint32 instanceOffset) {
    if (!vertexCount || !indexCount) return;
    if (!indexFill) { cp2077::web::FailBoundGpuCommand(); return; }
    const FillBufferFunc fill = [&indexFill](void* data, Uint32 bytes) { indexFill(static_cast<Uint16*>(data), bytes); };
    IssueSystemIndexedDraw(false, primitive, vertexCount, indexCount, layout, vertexFill, fill, instances, instanceOffset);
}
void DrawSystemIndexedPrimitive32(ePrimitiveType primitive, Uint32 vertexCount, Uint32 indexCount,
                                  VertexLayoutRef layout, const FillBufferFunc& vertexFill,
                                  const FillIndexBuffer32Func& indexFill, Uint32 instances, Uint32 instanceOffset) {
    if (!vertexCount || !indexCount) return;
    if (!indexFill) { cp2077::web::FailBoundGpuCommand(); return; }
    const FillBufferFunc fill = [&indexFill](void* data, Uint32 bytes) { indexFill(static_cast<Uint32*>(data), bytes); };
    IssueSystemIndexedDraw(true, primitive, vertexCount, indexCount, layout, vertexFill, fill, instances, instanceOffset);
}
