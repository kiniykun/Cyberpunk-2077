#include "common/gpuApi/include/gpuApiPublic.h"
#include "common/renderData/include/imageProcessing.h"
#include "../../cp2077SourceCode/extracted/root/R6.Root/Mainline/dev/external/tinyexr/tinyexr.h"
namespace GpuWrapApi {
// Original Windows wrapper body, enabled for the browser's real Emscripten FS.
Bool SaveImageAsEXR(const float *data, int width, int height, int components, const char *outfilename) {
    return (TINYEXR_SUCCESS == SaveEXR(data,width,height,components,outfilename));
}
}
