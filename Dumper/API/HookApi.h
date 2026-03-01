#pragma once

#include "Server/HttpServer.h"

namespace UExplorer::API
{
void RegisterHookRoutes(HttpServer& server);

// Ensure ProcessEvent VTable hook is installed (for GameThread dispatch)
bool EnsurePEHookInstalled();
} // namespace UExplorer::API
