#include "Router.h"
#include "StatusApi.h"
#include "ObjectsApi.h"
#include "ClassesApi.h"
#include "EnumsApi.h"
#include "DumpApi.h"
#include "MemoryApi.h"
#include "WorldApi.h"
#include "CallApi.h"
#include "BlueprintApi.h"
#include "WatchApi.h"
#include "HookApi.h"
#include "EventsApi.h"

namespace UExplorer::API
{

void RegisterAllRoutes(HttpServer& server)
{
	// Set server instance for SSE broadcasting
	SetServer(&server);

	RegisterStatusRoutes(server);
	RegisterObjectsRoutes(server);
	RegisterClassesRoutes(server);
	RegisterEnumsRoutes(server);
	RegisterDumpRoutes(server);
	RegisterMemoryRoutes(server);
	RegisterWorldRoutes(server);
	RegisterCallRoutes(server);
	RegisterBlueprintRoutes(server);
	RegisterWatchRoutes(server);
	RegisterHookRoutes(server);
	RegisterEventsRoutes(server);

	// Initialize hooks (installs PostRender VTable hook for game thread dispatch)
	InitHooks();
}

} // namespace UExplorer::API
