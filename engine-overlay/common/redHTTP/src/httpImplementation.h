/**
 * Copyright (c) 2020 CD Projekt Red. All Rights Reserved.
 */

#pragma once

#if defined( RED_PLATFORM_WEB )
#include "http-web.h"
namespace http { using Impl = WebImpl; }
#elif defined( RED_PLATFORM_WINPC )
#include "curl/curlHTTPAsyncClient.h"
namespace http { using Impl = CURLImpl; }
#elif defined( RED_PLATFORM_DURANGO )
#include "uwp/uwpHTTPAsyncClient.h"
namespace http { using Impl = UWPImpl; }
#elif defined( RED_PLATFORM_ORBIS )
#include "ps4/ps4HTTPAsyncClient.h"
namespace http { using Impl = PS4Impl; }
#elif defined( RED_PLATFORM_LINUX )
// TODO(#erykdwornicki #http): linux http client
#include "null/nullHTTPAsyncClient.h"
namespace http { using Impl = NULLImpl; }
#else
#error "Provide HTTP Async implementation"
#endif
