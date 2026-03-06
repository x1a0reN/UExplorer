#pragma once

#include "Server/HttpServer.h"

namespace UExplorer::API
{
void RegisterDumpRoutes(HttpServer& server);
void ShutdownDumpJobs();
} // namespace UExplorer::API
