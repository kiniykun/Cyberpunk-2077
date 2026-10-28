#!/usr/bin/env python3
"""[w32] gpu-shader-web.cpp (+ gpu-pipeline-warmup-web.inl) wasm32 copies: lookups for the compact packages produced by
gen-w32-shader-packages.py. Native bytecode matches by exact length + SHA-256; a matched table entry is returned as a
lazily materialized mirror whose wgsl is the inflated text (allocated once per shader actually used, then stable)."""
import subprocess
from pathlib import Path
PY = r'D:\Cyber punk\tools\emsdk\python\3.13.3_64bit\python.exe'; OV = r'D:\2077\work\w32\w32-overlay.py'
def copy(orig):
    subprocess.run([PY, OV, 'add', orig], capture_output=True, check=True)
    return Path(subprocess.run([PY, OV, 'resolve', orig], capture_output=True, text=True, check=True).stdout.strip())
def patch(orig, edits):
    p = copy(orig); raw = p.read_bytes().decode('utf-8'); crlf = '\r\n' in raw
    s = raw.replace('\r\n', '\n')
    if 'CpWebMaterialize' in s: print('already', p.name); return
    for a, b in edits:
        assert s.count(a) == 1, (p.name, a[:90], s.count(a)); s = s.replace(a, b)
    p.write_bytes((s.replace('\n', '\r\n') if crlf else s).encode('utf-8')); print('patched', p.name)

HELPERS = r'''
#if defined( __wasm32__ )
#include <atomic>
#include <cstdlib>
#include <mutex>
// [w32] Compact packages (work/w32/gen-w32-shader-packages.py): nativeBytecode = SHA-256 of the native DXIL,
// wgsl = [u32 compressedBytes][zlib]. Matched entries are materialized once into a stable mirror.
#include "D:/Cyber punk/extracted/root/R6.Root/Mainline/dev/src/common/redCompression/src/zlib/zlib.h" // engine zlib (Z_SOLO: no default allocators)
namespace {
voidpf CpWebZAlloc(voidpf, uInt n, uInt s){ return std::calloc(n, s); }
void CpWebZFree(voidpf, voidpf p){ std::free(p); }
bool CpWebInflate(unsigned char* out, std::size_t cap, const unsigned char* in, std::size_t n){
    z_stream z; std::memset(&z,0,sizeof z); z.zalloc=CpWebZAlloc; z.zfree=CpWebZFree;
    z.next_in=const_cast<unsigned char*>(in); z.avail_in=uInt(n); z.next_out=out; z.avail_out=uInt(cap);
    if(inflateInit(&z)!=Z_OK) return false;
    const int r=inflate(&z,Z_FINISH); const bool ok=(r==Z_STREAM_END && z.total_out==cap); inflateEnd(&z); return ok;
}
}
namespace {
struct CpWebSha256 {
    std::uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    static std::uint32_t R(std::uint32_t x,int n){return (x>>n)|(x<<(32-n));}
    void Block(const unsigned char* p){
        static const std::uint32_t k[64]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        std::uint32_t w[64];
        for(int i=0;i<16;++i) w[i]=std::uint32_t(p[i*4])<<24|std::uint32_t(p[i*4+1])<<16|std::uint32_t(p[i*4+2])<<8|p[i*4+3];
        for(int i=16;i<64;++i){std::uint32_t s0=R(w[i-15],7)^R(w[i-15],18)^(w[i-15]>>3),s1=R(w[i-2],17)^R(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
        std::uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for(int i=0;i<64;++i){std::uint32_t t1=hh+(R(e,6)^R(e,11)^R(e,25))+((e&f)^(~e&g))+k[i]+w[i],t2=(R(a,2)^R(a,13)^R(a,22))+((a&b)^(a&c)^(b&c));hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    void Digest(const void* data,std::size_t n,unsigned char out[32]){
        const unsigned char* p=static_cast<const unsigned char*>(data);std::size_t i=0;
        for(;i+64<=n;i+=64)Block(p+i);
        unsigned char tail[128]={0};std::size_t r=n-i;std::memcpy(tail,p+i,r);tail[r]=0x80;
        const std::size_t total=(r+9<=64)?64:128;const std::uint64_t bits=std::uint64_t(n)*8;
        for(int b=0;b<8;++b)tail[total-1-b]=static_cast<unsigned char>(bits>>(8*b));
        Block(tail);if(total==128)Block(tail+64);
        for(int j=0;j<8;++j){out[j*4]=h[j]>>24;out[j*4+1]=h[j]>>16;out[j*4+2]=h[j]>>8;out[j*4+3]=h[j];}
    }
};
template<std::size_t N> struct CpWebMirror {
    ConvertedGraphicsShader entries[N];
    std::atomic<bool> ready[N];
};
std::mutex& CpWebMaterializeLock(){static std::mutex m;return m;}
template<std::size_t N>
const ConvertedGraphicsShader* CpWebMaterializeIn(const ConvertedGraphicsShader (&table)[N], const ConvertedGraphicsShader* s, bool& found){
    if(s<std::begin(table)||s>=std::end(table)) return s;
    found=true;
    static CpWebMirror<N>* mirror=new CpWebMirror<N>();
    const std::size_t i=std::size_t(s-std::begin(table));
    if(mirror->ready[i].load(std::memory_order_acquire)) return &mirror->entries[i];
    std::lock_guard<std::mutex> lock(CpWebMaterializeLock());
    if(!mirror->ready[i].load(std::memory_order_relaxed)){
        ConvertedGraphicsShader m=*s;
        if(s->wgsl){
            const unsigned char* blob=reinterpret_cast<const unsigned char*>(s->wgsl);
            std::uint32_t compressed=0;std::memcpy(&compressed,blob,4);
            char* text=static_cast<char*>(std::malloc(s->wgslBytes+1));
            const std::size_t out=s->wgslBytes;
            if(!text||!CpWebInflate(reinterpret_cast<unsigned char*>(text),out,blob+4,compressed)){
                std::fprintf(stderr,"CP2077 w32 shader package: WGSL inflate failed for %s\n",s->originalHash);std::abort();
            }
            text[out]=0;m.wgsl=text;
        }
        mirror->entries[i]=m;mirror->ready[i].store(true,std::memory_order_release);
    }
    return &mirror->entries[i];
}
}
const ConvertedGraphicsShader* CpWebMaterialize(const ConvertedGraphicsShader* s){
    if(!s) return s;
    bool found=false;
    s=CpWebMaterializeIn(kConvertedMaterialGraphicsShaders,s,found); if(found) return s;
    s=CpWebMaterializeIn(kConvertedGraphicsShaders,s,found); if(found) return s;
    s=CpWebMaterializeIn(kConvertedStaticGraphicsShaders,s,found);
    return s;
}
#endif
'''
SRC = r'D:\2077\src\gpu-shader-web.cpp'
patch(SRC, [
 ('#include "gpu-depth-gather-variant.inc"\n', '#include "gpu-depth-gather-variant.inc"\n' + HELPERS),
 ('''const ConvertedGraphicsShader* FindConvertedGraphicsShaderBytecode(const void* data, std::size_t bytes) {
    if (!data) return nullptr;
''', '''const ConvertedGraphicsShader* FindConvertedGraphicsShaderBytecode(const void* data, std::size_t bytes) {
    if (!data) return nullptr;
#if defined( __wasm32__ )
    { // [w32] exact length + SHA-256 of the native bytecode; hash computed once, only if some length matches
        unsigned char digest[32]; bool hashed=false;
        auto same=[&](const ConvertedGraphicsShader& shader){
            if(bytes!=shader.nativeBytecodeBytes) return false;
            if(!hashed){CpWebSha256().Digest(data,bytes,digest);hashed=true;}
            return std::memcmp(digest,shader.nativeBytecode,32)==0;
        };
        for (const auto& shader : kConvertedMaterialGraphicsShaders) if (same(shader)) return CpWebMaterialize(&shader);
        for (const auto& shader : kConvertedGraphicsShaders) if (same(shader)) return CpWebMaterialize(&shader);
        for (const auto& shader : kConvertedStaticGraphicsShaders) if (same(shader)) return CpWebMaterialize(&shader);
        return nullptr;
    }
#endif
'''),
 ('''std::uint32_t GraphicsFragmentOutputMask(const ConvertedGraphicsShader* shader) {
''', '''std::uint32_t GraphicsFragmentOutputMask(const ConvertedGraphicsShader* shader) {
#if defined( __wasm32__ )
    shader = CpWebMaterialize(shader); // [w32]
#endif
'''),
 ('''const ConvertedGraphicsShader* FindConvertedGraphicsLogicXorVariant(const ConvertedGraphicsShader* original){
''', '''const ConvertedGraphicsShader* FindConvertedGraphicsLogicXorVariant(const ConvertedGraphicsShader* original){
#if defined( __wasm32__ )
 original = CpWebMaterialize(original); // [w32]
#endif
'''),
 ('''const ConvertedGraphicsShader* FindConvertedGraphicsGiRGBOutputVariant(const ConvertedGraphicsShader* original) {
''', '''const ConvertedGraphicsShader* FindConvertedGraphicsGiRGBOutputVariant(const ConvertedGraphicsShader* original) {
#if defined( __wasm32__ )
    original = CpWebMaterialize(original); // [w32]
#endif
'''),
 ('''const ConvertedGraphicsShader* FindConvertedGraphicsDepthGatherVariant(const ConvertedGraphicsShader* original) {
''', '''const ConvertedGraphicsShader* FindConvertedGraphicsDepthGatherVariant(const ConvertedGraphicsShader* original) {
#if defined( __wasm32__ )
    original = CpWebMaterialize(original); // [w32]
#endif
'''),
])
patch(r'D:\2077\src\gpu-pipeline-warmup-web.inl', [
 ('''            if (hash != s->originalHash) continue;
            if (match(s)) return s;''', '''            if (hash != s->originalHash) continue;
#if defined( __wasm32__ )
            if (match(s)) return CpWebMaterialize(s); // [w32] compact package entry -> inflated mirror
            for (auto* v : {FindConvertedGraphicsGiRGBOutputVariant(s), FindConvertedGraphicsDepthGatherVariant(s),
                            FindConvertedGraphicsLogicXorVariant(s)})
                if (match(v)) return v;
            continue;
#endif
            if (match(s)) return s;'''),
])
