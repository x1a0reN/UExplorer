#pragma once

#include "Server/HttpServer.h"

namespace UExplorer::API
{
void RegisterHookRoutes(HttpServer& server);

// Initialize hooks on DLL load
void InitHooks();

// Cleanup hooks on DLL unload
void ShutdownHooks();

} // namespace UExplorer::API
