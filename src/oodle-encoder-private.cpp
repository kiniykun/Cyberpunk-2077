#include <emmintrin.h>
#include "D:/cp2077 port/tools/ooz-encoder/stdafx.h"
#include "D:/cp2077 port/tools/ooz-encoder/compress.h"
extern "C" int CpWebKrakenEncode(uint8* input,int length,uint8* output,int capacity,int level) {
    if(!input||!output||length<=0||length>128*1024*1024||(level!=4&&level!=6))return 0;
    const int bound=length+274*((length+262143)/262144);
    if(capacity<bound)return 0;
    const int result=CompressBlock(kCompressorKraken,input,output,length,level,nullptr,nullptr,nullptr);
    return result>0&&result<=bound?result:0;
}
