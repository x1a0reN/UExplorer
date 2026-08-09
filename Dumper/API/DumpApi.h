#pragma once

#include "Server/HttpServer.h"

namespace UExplorer::API
{
void RegisterDumpRoutes(HttpServer& server);
bool ShutdownDumpJobs(int timeoutMs = 5000);
} // namespace UExplorer::API
