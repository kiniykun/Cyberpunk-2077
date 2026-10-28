#!/usr/bin/env python3
"""[w32] memory-web.cpp (wasm32 copy only) for the iPhone budget.

1. Zero-fill without touching untouched pages. Commit must hand out zeroed pages (unchanged contract), but a page that
   was never written is already zero: Wasm memory.grow returns zero pages. A range records the sbrk top observed just
   before its backing was allocated; pages at or above it are fresh. A per-page 'dirty' flag says whether a page may
   hold non-zero bytes: fresh pages start clean, pages below the old top start dirty. Commit zero-fills only dirty
   pages; decommit no longer memsets (it marks the page dirty, so the next commit zero-fills it lazily). Releasing a
   range is unchanged. Net: committed-but-unused pages are no longer touched, so they never become resident.
2. Attribution: ranges >= 1 MiB remember the first engine frames of the reserving call stack; the probe prints
   committed bytes grouped by that site (top 24) so budgets target real owners.
"""
from pathlib import Path
p = Path(r'D:\2077\build-w32\src-w32\port\src\memory-web.cpp')
s = p.read_text(encoding='utf-8')
if '[w32] dirty' in s: print('already'); raise SystemExit
def rep(a, b):
    global s
    assert s.count(a) == 1, a[:80]; s = s.replace(a, b)
rep('    SystemAllocatorWeb** committed;\n',
    '    SystemAllocatorWeb** committed;\n    unsigned char* dirty; // [w32] dirty: page may hold non-zero bytes (fresh grown pages are zero)\n    const char* site;     // [w32] reserving call site (ranges >= 1 MiB), for the memory probe\n')
rep('    void* backing=record->committed?emscripten_builtin_memalign(alignment,size_t(count)):nullptr;',
    '''#if defined( __wasm32__ )
    record->dirty=record->committed?static_cast<unsigned char*>(emscripten_builtin_malloc(size_t(count/Page))):nullptr;
    const u64 freshFrom=reinterpret_cast<u64>(sbrk(0)); // [w32] pages at/above the pre-allocation top were never touched
    void* backing=record->dirty?emscripten_builtin_memalign(alignment,size_t(count)):nullptr;
    if(backing){
        for(u64 i=0;i<count/Page;++i) record->dirty[i]=(reinterpret_cast<u64>(backing)+i*Page<freshFrom)?1:0;
        if(count>=(1ull<<20)){ // first engine frames of the reserving stack
            static thread_local char stack[4096]; emscripten_get_callstack(EM_LOG_JS_STACK,stack,sizeof(stack));
            char site[256]={0};unsigned used=0,frames=0;
            for(char* q=stack;frames<3&&(q=std::strstr(q,"cp2077.wasm."));){
                q+=12;char* e=std::strchr(q,'(');char* nl=std::strchr(q,'\\n');if(!e||(nl&&nl<e))e=nl?nl:q+std::strlen(q);
                const unsigned len=unsigned(e-q);
                if(std::strstr(q,"red::memory::")==q && frames==0 && std::strncmp(q,"red::memory::SystemPageAllocatorWeb",35)==0) continue;
                if(std::strncmp(q,"red::memory::System",19)==0) continue;
                if(used+len+3<sizeof(site)){ if(used){site[used++]='<';} std::memcpy(site+used,q,len);used+=len;++frames; }
                else break;
            }
            char* copy=static_cast<char*>(emscripten_builtin_malloc(used+1)); if(copy){std::memcpy(copy,site,used);copy[used]=0;} record->site=copy;
        }
    }
#else
    void* backing=record->committed?emscripten_builtin_memalign(alignment,size_t(count)):nullptr;
#endif''')
rep('    std::memset(reinterpret_cast<void*>(block.address),0,size_t(bytes));\n    for(u64 i=first;i<first+count;++i)r->committed[i]=this;',
    '''#if defined( __wasm32__ )
    for(u64 i=first;i<first+count;++i) if(r->dirty[i]){ std::memset(reinterpret_cast<void*>(r->begin+i*Page),0,size_t(Page)); r->dirty[i]=0; }
    for(u64 i=first;i<first+count;++i){ r->committed[i]=this; r->dirty[i]=1; } // [w32] the owner may write it from now on
#else
    std::memset(reinterpret_cast<void*>(block.address),0,size_t(bytes));
    for(u64 i=first;i<first+count;++i)r->committed[i]=this;
#endif''')
rep('    std::memset(reinterpret_cast<void*>(b.address),0,size_t(b.size));\n    for(u64 i=first;i<first+count;++i)r->committed[i]=nullptr;',
    '''#if defined( __wasm32__ )
    for(u64 i=first;i<first+count;++i)r->committed[i]=nullptr; // [w32] stays dirty; zero-filled lazily on recommit
#else
    std::memset(reinterpret_cast<void*>(b.address),0,size_t(b.size));
    for(u64 i=first;i<first+count;++i)r->committed[i]=nullptr;
#endif''')
# release frees the side arrays
rep('    emscripten_builtin_free(r->committed);emscripten_builtin_free(r);\n}',
    '    emscripten_builtin_free(r->committed);\n#if defined( __wasm32__ )\n    emscripten_builtin_free(r->dirty);emscripten_builtin_free(const_cast<char*>(r->site));\n#endif\n    emscripten_builtin_free(r);\n}')
# probe: grouped by site
rep('''    std::fprintf(stderr,"CP2077 w32 memory probe:''', '''    {
        struct Agg { const char* site; u64 bytes; unsigned n; };
        static Agg agg[512]; unsigned na=0; u64 small=0;
        Lock lock;
        for(Range* r=ranges;r;r=r->next){
            if(!r->site){small+=r->committedBytes;continue;}
            unsigned k=0; while(k<na && std::strcmp(agg[k].site,r->site)!=0) ++k;
            if(k==na){ if(na==512){small+=r->committedBytes;continue;} agg[na++]={r->site,0,0}; }
            agg[k].bytes+=r->committedBytes; agg[k].n++;
        }
        for(unsigned i=0;i<na;++i) for(unsigned j=i+1;j<na;++j) if(agg[j].bytes>agg[i].bytes){Agg t=agg[i];agg[i]=agg[j];agg[j]=t;}
        for(unsigned i=0;i<na&&i<24;++i) std::fprintf(stderr,"CP2077 w32 memory site: committedMiB=%.1f ranges=%u site=%s\\n",agg[i].bytes/1048576.0,agg[i].n,agg[i].site);
        std::fprintf(stderr,"CP2077 w32 memory site: committedMiB=%.1f site=<ranges below 1 MiB>\\n",small/1048576.0);
    }
    std::fprintf(stderr,"CP2077 w32 memory probe:''')
p.write_text(s, encoding='utf-8'); print('patched')
