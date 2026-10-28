#include "D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/dev/external/oodle/include/oodle2.h"
#include <cstdint>
#include <limits>
// Recovered original SDK formulas, with overflow guards for invalid inputs.
// These preserve allocation bounds; they do not supply a compressor.
SINTa OodleLZ_GetCompressedBufferSizeNeeded(SINTa rawSize) {
    if(rawSize<0)return 0;
    constexpr SINTa block=262144,overhead=274;
    const SINTa blocks=rawSize/block+(rawSize%block!=0);
    if(blocks>(std::numeric_limits<SINTa>::max()-rawSize)/overhead)return 0;
    return rawSize+blocks*overhead;
}
S32 OodleLZDecoder_MemorySizeNeeded(OodleLZ_Compressor compressor,SINTa rawLength) {
    // The active engine path and isolated worker are Kraken-only. Other codec
    // workspace layouts are unsupported rather than assigned fabricated sizes.
    if(compressor!=OodleLZ_Compressor_Kraken || rawLength<-1)return 0;
    constexpr SINTa maximumScratch=442368,constantScratch=53280,base=184;
    if(rawLength<=0 || rawLength>=(maximumScratch-constantScratch+2)/3)
        return static_cast<S32>(base+maximumScratch);
    return static_cast<S32>(base+3*rawLength+constantScratch);
}
