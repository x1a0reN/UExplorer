#pragma once

#include "Server/HttpServer.h"

namespace UExplorer::API
{
void RegisterHookRoutes(HttpServer& server);

// Initialize the game-thread pump. ProcessEvent monitoring remains lazy.
bool InitHooks();

// Cleanup hooks on DLL unload
bool ShutdownHooks();

} // namespace UExplorer::API
