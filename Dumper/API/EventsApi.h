#pragma once

#include "Server/HttpServer.h"
#include <string>

namespace UExplorer::API
{

// Register SSE event routes
void RegisterEventsRoutes(HttpServer& server);

// SSE event broadcasting (called from HookApi and WatchApi)
void BroadcastHookEvent(const std::string& hookName, const std::string& data);
void BroadcastWatchEvent(int watchId, const std::string& data);
void BroadcastLogEvent(const std::string& level, const std::string& message);

// Set the HTTP server instance for broadcasting
void SetServer(HttpServer* server);

} // namespace UExplorer::API
