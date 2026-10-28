#pragma once
#include "D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/dev/external/gog/Include/galaxy/Errors.h"
#include "D:/cp2077SourceCode/extracted/root/R6.Root/Mainline/dev/external/gog/Include/galaxy/IListenerRegistrar.h"
#include <functional>
#include <vector>
namespace cpweb {
void SetGalaxyError(galaxy::api::IError::Type type,const char* message);
void ClearGalaxyError();
std::vector<galaxy::api::IGalaxyListener*> GalaxyListeners(galaxy::api::ListenerType);
bool QueueGalaxyCallback(std::function<void()>);
void ProcessGalaxyCallbacks();
void DiscardGalaxyCallbacks();
}
