#pragma once
#include "errorHandlerImplHooks.h"
namespace red { namespace err {
ErrorHandlerHooks RegisterErrorHooksOnceWeb(Uint32 flags, const char* version,
    ErrorHandlerHooks::ScriptCallstackVisitorFunc* visitor);
}}
