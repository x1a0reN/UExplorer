#pragma once

#include "Server/HttpServer.h"

namespace UExplorer::API
{

// Register all API routes on the server
void RegisterAllRoutes(HttpServer& server);

} // namespace UExplorer::API
