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

namespace UExplorer::API
{

void RegisterAllRoutes(HttpServer& server)
{
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
}

} // namespace UExplorer::API
