#pragma once

#if defined( RED_PLATFORM_WINPC )

#include "win/AsyncFileImpl.h"

REC_CORE_NAMESPACE_BEGIN
IO_LAYER_NAMESPACE_BEGIN

namespace platformAPI = win;

IO_LAYER_NAMESPACE_END
REC_CORE_NAMESPACE_END

#elif defined( RED_PLATFORM_ORBIS )

#include "orbis/AsyncFileImpl.h"

REC_CORE_NAMESPACE_BEGIN
IO_LAYER_NAMESPACE_BEGIN

namespace platformAPI = orbis;

IO_LAYER_NAMESPACE_END
REC_CORE_NAMESPACE_END

#elif defined( RED_PLATFORM_DURANGO )

#include "durango/AsyncFileImpl.h"

REC_CORE_NAMESPACE_BEGIN
IO_LAYER_NAMESPACE_BEGIN

namespace platformAPI = durango;

IO_LAYER_NAMESPACE_END
REC_CORE_NAMESPACE_END

#elif defined(RED_PLATFORM_WEB)
#include "recording-file-web.h"
namespace recCore { namespace ioLayer { namespace platformAPI = web; }}
#else
#error  "Unsuported platform!"
#endif