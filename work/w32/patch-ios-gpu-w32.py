#!/usr/bin/env python3
"""[w32-iosgpu] Phase 4 GPU/mapped-memory work for the wasm32 iPhone profile (?mem=ios), w32 overlay copies only.

1. Mapped shadow drop: write-tracked mapped upload buffers (skinning 40 MiB + 3 small, ~49 MiB) keep a CPU shadow
   (committedMappedBytes) only for a one-time full diff. Under mem=ios the shadow is freed when tracking starts; every
   page is marked dirty once so the next flush uploads the whole buffer through the paged path, then only written pages.
2. Diagnostics (mem=ios only): one stack line per GPU buffer >= 4 MiB and texture >= 4 MiB created, to name the owners
   of the large engine pools.
"""
import subprocess
from pathlib import Path
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'; OV = r'D:\2077\work\w32\w32-overlay.py'
def copy(orig):
    subprocess.run([PY, OV, 'add', orig], capture_output=True, check=True)
    return Path(subprocess.run([PY, OV, 'resolve', orig], capture_output=True, text=True, check=True).stdout.strip())
def patch(orig, edits, tag):
    p = copy(orig); assert 'build-w32' in str(p), p
    raw = p.read_bytes().decode('utf-8', errors='surrogateescape'); crlf = '\r\n' in raw
    s = raw.replace('\r\n', '\n')
    if tag in s: print('already', p.name); return
    for a, b in edits:
        assert s.count(a) == 1, (p.name, a[:90], s.count(a)); s = s.replace(a, b)
    p.write_bytes((s.replace('\n', '\r\n') if crlf else s).encode('utf-8', errors='surrogateescape')); print('patched', p.name)

STACK_FN = r'''
extern "C" int CpWebMemIos();
// [w32-iosgpu] mem=ios diagnostic: names the engine owner of a large GPU allocation (wasm function names from the JS stack).
static void CpWebBigGpuAllocNote(const char* kind, unsigned long long bytes, const char* what) {
    if (!CpWebMemIos()) return;
    static thread_local char stack[12288]; char names[700] = {0}; unsigned used = 0, frames = 0;
    emscripten_get_callstack(EM_LOG_JS_STACK, stack, sizeof(stack));
    for (char* q = stack; frames < 9 && (q = std::strstr(q, "cp2077.wasm.")); ) {
        q += 12; char* e = std::strchr(q, '('); char* nl = std::strchr(q, '\n'); if (!e || (nl && nl < e)) e = nl ? nl : q + std::strlen(q);
        const unsigned len = unsigned(e - q);
        if (!len || !std::strncmp(q, "CpWebBigGpuAllocNote", 20) || !std::strncmp(q, "emscripten", 10) || !std::strncmp(q, "GpuApi::Create", 14)) continue;
        if (used + len + 3 >= sizeof(names)) break;
        if (used) { names[used++] = ' '; names[used++] = '<'; names[used++] = ' '; }
        std::memcpy(names + used, q, len); used += len; ++frames;
    }
    std::fprintf(stderr, "CP2077 w32 big gpu alloc: kind=%s MiB=%.1f %s :: %s\n", kind, bytes / 1048576.0, what, names);
}
'''

patch(r'D:\2077\src\gpu-api-buffer-web.cpp', [
 ('''    std::vector<unsigned char> committedMappedBytes;
''', '''    std::vector<unsigned char> committedMappedBytes;
    bool shadowDropped = false; // [w32-iosgpu] mem=ios: write-tracked buffer without CPU shadow (paged uploads only)
'''),
 ('''namespace {
struct BufferEntry {''', STACK_FN + '''namespace {
struct BufferEntry {'''),
 ('''    auto native = cp2077::web::CreateGpuBuffer(descriptor.size, usage, data, data ? descriptor.size : 0, cpLabel);''',
  '''    if (descriptor.size >= (4u << 20)) { char what[96]; std::snprintf(what, sizeof(what), "%s usage=%u category=%u group=%u stride=%u",
        cpLabel, unsigned(descriptor.usage), unsigned(descriptor.category), unsigned(descriptor.group), unsigned(descriptor.structureStride));
        CpWebBigGpuAllocNote("buffer", descriptor.size, what); } // [w32-iosgpu]
    auto native = cp2077::web::CreateGpuBuffer(descriptor.size, usage, data, data ? descriptor.size : 0, cpLabel);'''),
 ('''        entry.liveRangeFullScanPending = true; // one full diff covers any write made before tracking began
        std::fprintf(stderr, "CP2077 skperf mapTrack: buffer %u (%zu bytes, %zu pages) write-tracked\\n", unsigned(reference.ID()), bytes, pages);''',
  '''        entry.liveRangeFullScanPending = true; // one full diff covers any write made before tracking began
        if (CpWebMemIos()) {
            // [w32-iosgpu] no shadow: every page dirty once (whole buffer uploaded by the paged path), then written pages only
            for (std::size_t i = 0; i < words; ++i) entry.dirtyWords[i].store(~std::uint64_t(0), std::memory_order_relaxed);
            entry.liveRangeFullScanPending = false; entry.shadowDropped = true;
            std::vector<unsigned char>().swap(entry.committedMappedBytes);
        }
        std::fprintf(stderr, "CP2077 skperf mapTrack: buffer %u (%zu bytes, %zu pages) write-tracked shadow=%d\\n", unsigned(reference.ID()), bytes, pages, entry.shadowDropped ? 0 : 1);'''),
 ('''    if (!entry.liveRangeTracked) {
        // Opting in must not lose bytes written before tracking began: the next flush still scans the whole
        // buffer once, then only live ranges.
        entry.liveRangeTracked = true;
        entry.liveRangeFullScanPending = true;
    }''', '''    if (!entry.liveRangeTracked) {
        // Opting in must not lose bytes written before tracking began: the next flush still scans the whole
        // buffer once, then only live ranges.
        entry.liveRangeTracked = true;
        entry.liveRangeFullScanPending = !entry.shadowDropped; // [w32-iosgpu] shadowless buffers were fully marked dirty
    }'''),
 ('''            if (entry.committedMappedBytes.size()!=entry.mappedBytes.size() || entry.references == INT32_MAX) {''',
  '''            if ((!entry.shadowDropped && entry.committedMappedBytes.size()!=entry.mappedBytes.size()) || entry.references == INT32_MAX) {'''),
 ('''            Scan scan{id, entry.buffer, entry.mappedBytes.data(), entry.committedMappedBytes.data(), entry.mappedBytes.size(),''',
  '''            Scan scan{id, entry.buffer, entry.mappedBytes.data(), entry.shadowDropped ? nullptr : entry.committedMappedBytes.data(), entry.mappedBytes.size(),'''),
 ('''                scan.trackedPending = entry.liveRangeFullScanPending;
                if (!scan.trackedPending) { scan.paged = true; scan.live = true; }''',
  '''                scan.trackedPending = entry.liveRangeFullScanPending && !entry.shadowDropped;
                if (!scan.trackedPending) { scan.paged = true; scan.live = true; scan.fullScanPending = false; }'''),
 ('''        if (scan.paged && MapNoDiffOn()) {''', '''        if (scan.paged && (MapNoDiffOn() || !scan.committed)) { // [w32-iosgpu] shadowless: always paged'''),
 ('''            if (!(scan.paged && MapNoDiffOn())) std::memcpy(scan.committed + patch.offset, patch.bytes.data(), patch.bytes.size()); // shadow unused for tracked pages''',
  '''            if (scan.committed && !(scan.paged && MapNoDiffOn())) std::memcpy(scan.committed + patch.offset, patch.bytes.data(), patch.bytes.size()); // shadow unused for tracked pages'''),
 ('''            if(patch.offset+patch.bytes.size()>scan.bytes){success=false;continue;}
            std::memcpy(scan.committed+patch.offset,patch.bytes.data(),patch.bytes.size());''',
  '''            if(patch.offset+patch.bytes.size()>scan.bytes){success=false;continue;}
            if(scan.committed)std::memcpy(scan.committed+patch.offset,patch.bytes.data(),patch.bytes.size()); // [w32-iosgpu]'''),
], '[w32-iosgpu]')

patch(r'D:\2077\src\gpu-api-texture-web.cpp', [
 ('''TextureRef CreateTexture(const TextureDesc& descriptor, const TextureInitData& initial) {
    auto format = TranslateResource(descriptor);''', '''TextureRef CreateTexture(const TextureDesc& descriptor, const TextureInitData& initial) {
    auto format = TranslateResource(descriptor);
    { // [w32-iosgpu] mem=ios diagnostic for large textures
        const unsigned long long bytes = TextureStorageBytes(descriptor);
        if (bytes >= (4ull << 20)) { char what[112]; std::snprintf(what, sizeof(what), "fmt=%u type=%u %ux%ux%u mips=%u usage=%u group=%u",
            unsigned(descriptor.format), unsigned(descriptor.type), descriptor.width, descriptor.height, descriptor.depthOrSliceNum,
            unsigned(descriptor.numMips), unsigned(descriptor.usage), unsigned(descriptor.group));
            CpWebBigGpuTexNote(bytes, what); }
    }'''),
 ('''TextureRef CreateTexture(const TextureDesc& descriptor, const TextureInitData& initial) {''',
  STACK_FN.replace('CpWebBigGpuAllocNote', 'CpWebBigGpuTexNoteImpl') + '''static void CpWebBigGpuTexNote(unsigned long long bytes, const char* what) { CpWebBigGpuTexNoteImpl("texture", bytes, what); }
TextureRef CreateTexture(const TextureDesc& descriptor, const TextureInitData& initial) {'''),
], '[w32-iosgpu]')
