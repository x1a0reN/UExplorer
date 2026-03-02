#include "WinMemApi.h"

#include "HookApi.h"
#include "ApiCommon.h"
#include "GameThreadQueue.h"
#include "EventsApi.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/UnrealObjects.h"
#include "Unreal/UnrealTypes.h"
#include "Unreal/Enums.h"
#include "OffsetFinder/Offsets.h"

#include <format>
#include <mutex>
#include <map>
#include <set>
#include <atomic>
#include <vector>
#include <deque>
#include <chrono>

namespace UExplorer::API
{

// ── Hook entry ──────────────────────────────────────────────
struct HookEntry
{
	int Id;
	std::string FunctionPath;
	std::vector<void*> FunctionAddresses;  // ALL matching UFunction objects
	bool Enabled;
	int64_t HitCount;
	int64_t LastHitTime;
	int64_t CreatedTime;
};

struct HookLogEntry
{
	int HookId;
	std::string FunctionName;
	std::string CallerName;
	int64_t Timestamp;
};

// ── Globals ─────────────────────────────────────────────────
static std::mutex g_HookMutex;
static std::map<int, HookEntry> g_Hooks;
static std::atomic<int> g_HookCounter{0};

static std::mutex g_LogMutex;
static std::deque<HookLogEntry> g_HookLog;
static constexpr size_t MAX_LOG_SIZE = 2000;

// Set of monitored UFunction addresses for fast lookup in hot path
static std::mutex g_MonitorMutex;
static std::set<void*> g_MonitoredFunctions;

// ── VTable Hook infrastructure ───────────────────────────────
// Hook GameViewportClient::PostRender instead of ProcessEvent
// This avoids code integrity detection that restores inline detour patches
static void* g_GVCPtr = nullptr;              // GameViewportClient instance
static void** g_GVCVft = nullptr;             // Original VTable pointer
static void* g_OrigPostRender = nullptr;      // Original PostRender function
static bool g_VTableHookInstalled = false;

// Our PostRender hook - processes game thread queue every frame
typedef void (*PostRenderFn)(void*, void*);
static void HookedPostRender(void* InGVCCDO, void* InCanvas)
{
	// Process any pending game-thread dispatch calls
	GameThread::ProcessQueue();

	// Call original PostRender
	auto Orig = reinterpret_cast<PostRenderFn>(g_OrigPostRender);
	if (Orig)
		Orig(InGVCCDO, InCanvas);
}

static bool InstallPostRenderHook()
{
	if (g_VTableHookInstalled) return true;

	// Find GameViewportClient class
	UEObject GVCClass;
	for (int i = 0; i < ObjectArray::Num() && i < 10000; i++)
	{
		UEObject obj = ObjectArray::GetByIndex(i);
		if (!obj) continue;
		std::string name = obj.GetName();
		if (name == "GameViewportClient")
		{
			GVCClass = obj;
			break;
		}
	}

	if (!GVCClass)
	{
		std::cerr << "[HookApi] GameViewportClient class not found" << std::endl;
		return false;
	}

	// Get CDO (default object)
	UEObject CDO = GVCClass.Cast<UEClass>().GetDefaultObject();
	if (!CDO)
	{
		std::cerr << "[HookApi] GameViewportClient CDO not found" << std::endl;
		return false;
	}

	g_GVCPtr = CDO.GetAddress();
	if (!g_GVCPtr)
	{
		std::cerr << "[HookApi] Invalid GVC pointer" << std::endl;
		return false;
	}

	// Get VTable from CDO
	g_GVCVft = *reinterpret_cast<void***>(g_GVCPtr);
	if (!g_GVCVft)
	{
		std::cerr << "[HookApi] Invalid GVC VTable" << std::endl;
		return false;
	}

	// Get PostRender index
	int32 postRenderIdx = Off::InSDK::PostRender::GVCPostRenderIndex;
	if (postRenderIdx < 0)
	{
		std::cerr << "[HookApi] PostRender index not found" << std::endl;
		return false;
	}

	// Save original function
	g_OrigPostRender = g_GVCVft[postRenderIdx];
	if (!g_OrigPostRender)
	{
		std::cerr << "[HookApi] Original PostRender not found" << std::endl;
		return false;
	}

	// Make page writable
	DWORD oldProtect;
	if (!VirtualProtect(g_GVCVft + postRenderIdx, sizeof(void*), PAGE_READWRITE, &oldProtect))
	{
		std::cerr << "[HookApi] VirtualProtect failed" << std::endl;
		return false;
	}

	// Hook VTable
	g_GVCVft[postRenderIdx] = reinterpret_cast<void*>(&HookedPostRender);

	// Restore protection
	VirtualProtect(g_GVCVft + postRenderIdx, sizeof(void*), oldProtect, &oldProtect);

	g_VTableHookInstalled = true;

	// Get global UObject::ProcessEvent for game thread queue
	void* globalPE = nullptr;
	UEClass UObjectClass = ObjectArray::FindClassFast("Object");
	if (UObjectClass) {
		UEObject UObjectCDO = UObjectClass.GetDefaultObject();
		if (UObjectCDO) {
			void* cdoAddr = UObjectCDO.GetAddress();
			void** cdoVft = *reinterpret_cast<void***>(cdoAddr);
			int32 peIdx = Off::InSDK::ProcessEvent::PEIndex;
			if (peIdx > 0 && peIdx < 200 && cdoVft) {
				globalPE = cdoVft[peIdx];
				std::cerr << "[HookApi] Global ProcessEvent: " << std::hex << globalPE << std::dec << std::endl;
			}
		}
	}

	// Enable game thread queue with the global ProcessEvent
	if (globalPE) {
		auto PE = reinterpret_cast<void(*)(void*, void*, void*)>(globalPE);
		GameThread::Enable(PE);
	} else {
		std::cerr << "[HookApi] Warning: Could not get global ProcessEvent" << std::endl;
		GameThread::Enable([](void*, void*, void*) {});
	}

	std::cerr << "[HookApi] VTable hook installed: PostRender index=" << postRenderIdx << std::endl;
	return true;
}

static void UninstallPostRenderHook()
{
	if (!g_VTableHookInstalled) return;

	std::cerr << "[HookApi] Uninstalling PostRender hook..." << std::endl;

	// Disable game thread queue first to stop processing
	GameThread::Disable();

	// Try to restore VTable
	int32 postRenderIdx = Off::InSDK::PostRender::GVCPostRenderIndex;
	if (postRenderIdx >= 0 && g_GVCVft && g_OrigPostRender)
	{
		DWORD oldProtect;
		if (VirtualProtect(g_GVCVft + postRenderIdx, sizeof(void*), PAGE_READWRITE, &oldProtect))
		{
			g_GVCVft[postRenderIdx] = g_OrigPostRender;
			VirtualProtect(g_GVCVft + postRenderIdx, sizeof(void*), oldProtect, &oldProtect);
			std::cerr << "[HookApi] VTable restored" << std::endl;
		}
		else
		{
			std::cerr << "[HookApi] VirtualProtect failed, skipping VTable restore" << std::endl;
		}
	}

	g_VTableHookInstalled = false;
	g_GVCPtr = nullptr;
	g_GVCVft = nullptr;
	g_OrigPostRender = nullptr;

	std::cerr << "[HookApi] Hook uninstalled" << std::endl;
}

// ── Monitored function hook (inline detour on ProcessEvent) ───
// Still keep this for monitoring specific functions
static void* g_OriginalPE = nullptr;
static bool g_MonitorHookInstalled = false;
static std::atomic<int64_t> g_TotalPECalls{0};

static int64_t HookNowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

// ── Our ProcessEvent hook for monitoring (if needed) ──
static void HookedProcessEvent(void* Object, void* Function, void* Params)
{
	g_TotalPECalls.fetch_add(1, std::memory_order_relaxed);

	// Fast path: check if this function is monitored
	bool monitored = false;
	{
		std::lock_guard<std::mutex> lk(g_MonitorMutex);
		monitored = g_MonitoredFunctions.count(Function) > 0;
	}

	if (monitored)
	{
		std::string funcName, callerName;
		try {
			UEFunction f(static_cast<uint8_t*>(Function));
			funcName = f.GetName();
		} catch (...) {}

		int64_t now = HookNowMs();
		{
			std::lock_guard<std::mutex> hlk(g_HookMutex);
			for (auto& [id, h] : g_Hooks)
			{
				if (!h.Enabled) continue;
				for (void* fa : h.FunctionAddresses) {
					if (fa == Function) { h.HitCount++; h.LastHitTime = now; break; }
				}
			}
		}
		{
			std::lock_guard<std::mutex> llk(g_LogMutex);
			g_HookLog.push_back({ 0, funcName, callerName, now });
			if (g_HookLog.size() > MAX_LOG_SIZE)
				g_HookLog.pop_front();
		}

		// Broadcast SSE event
		try {
			json evt;
			evt["function"] = funcName;
			evt["timestamp"] = now;
			BroadcastHookEvent(funcName, evt.dump());
		} catch (...) {}
	}

	// Call original ProcessEvent
	if (g_OriginalPE)
		reinterpret_cast<void(*)(void*, void*, void*)>(g_OriginalPE)(Object, Function, Params);
}

// ── Hook API implementation ─────────────────────────────────

static bool InstallHook(const std::string& functionPath)
{
	// Parse function path (e.g., "Class.Function")
	size_t dotPos = functionPath.rfind('.');
	if (dotPos == std::string::npos) return false;

	std::string className = functionPath.substr(0, dotPos);
	std::string funcName = functionPath.substr(dotPos + 1);

	// Find class
	UEClass cls;
	for (int i = 0; i < ObjectArray::Num() && i < 10000; i++)
	{
		UEObject obj = ObjectArray::GetByIndex(i);
		if (!obj) continue;
		if (obj.GetName() == className && obj.IsA(EClassCastFlags::Class))
		{
			cls = obj.Cast<UEClass>();
			break;
		}
	}

	if (!cls) return false;

	// Find function
	UEFunction func = cls.GetFunction(className, funcName);
	if (!func) return false;

	void* funcAddr = func.GetAddress();
	if (!funcAddr) return false;

	// Add to monitored set
	{
		std::lock_guard<std::mutex> lk(g_MonitorMutex);
		g_MonitoredFunctions.insert(funcAddr);
	}

	return true;
}

static void UninstallHook(int id)
{
	std::lock_guard<std::mutex> lk(g_HookMutex);
	auto it = g_Hooks.find(id);
	if (it != g_Hooks.end())
	{
		for (void* addr : it->second.FunctionAddresses)
		{
			std::lock_guard<std::mutex> mlk(g_MonitorMutex);
			g_MonitoredFunctions.erase(addr);
		}
		g_Hooks.erase(it);
	}
}

static bool InstallPEHook()
{
	// Don't install inline detour anymore - use VTable hook instead
	// Just ensure PostRender hook is installed
	return InstallPostRenderHook();
}

static void UninstallPEHook()
{
	UninstallPostRenderHook();
}

static bool AddHook(const std::string& functionPath, int& outId)
{
	if (!InstallHook(functionPath)) return false;

	std::lock_guard<std::mutex> lk(g_HookMutex);
	int id = ++g_HookCounter;

	HookEntry entry;
	entry.Id = id;
	entry.FunctionPath = functionPath;
	entry.Enabled = true;
	entry.HitCount = 0;
	entry.LastHitTime = 0;
	entry.CreatedTime = HookNowMs();

	// Find function address
	size_t dotPos = functionPath.rfind('.');
	if (dotPos != std::string::npos)
	{
		std::string className = functionPath.substr(0, dotPos);
		std::string funcName = functionPath.substr(dotPos + 1);

		for (int i = 0; i < ObjectArray::Num() && i < 10000; i++)
		{
			UEObject obj = ObjectArray::GetByIndex(i);
			if (!obj) continue;
			if (obj.GetName() == className && obj.IsA(EClassCastFlags::Class))
			{
				UEClass cls = obj.Cast<UEClass>();
				UEFunction func = cls.GetFunction(className, funcName);
				if (func)
				{
					entry.FunctionAddresses.push_back(func.GetAddress());
				}
				break;
			}
		}
	}

	g_Hooks[id] = entry;
	outId = id;
	return true;
}

// ── HTTP Routes ───────────────────────────────────────────

void RegisterHookRoutes(HttpServer& server)
{
	// POST /api/v1/hooks/add — add a hook
	server.Post("/api/v1/hooks/add", [](const HttpRequest& req) -> HttpResponse {
		try {
			json body = json::parse(req.Body);
			std::string functionPath = body.value("function_path", "");

			if (functionPath.empty())
				return { 400, "application/json", MakeError("Missing function_path") };

			int hookId;
			if (!AddHook(functionPath, hookId))
				return { 400, "application/json", MakeError("Failed to add hook") };

			json data;
			data["id"] = hookId;
			data["function_path"] = functionPath;
			data["enabled"] = true;
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (const json::exception& e) {
			return { 400, "application/json", MakeError(std::string("Bad JSON: ") + e.what()) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to add hook") };
		}
	});

	// DELETE /api/v1/hooks/:id — remove a hook
	server.Delete("/api/v1/hooks/:id", [](const HttpRequest& req) -> HttpResponse {
		try {
			auto params = ParseQuery(req.Query);
			int id = std::stoi(params["id"]);

			UninstallHook(id);

			json data;
			data["removed"] = true;
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 400, "application/json", MakeError("Invalid hook id") };
		}
	});

	// PATCH /api/v1/hooks/:id — enable/disable hook
	server.Patch("/api/v1/hooks/:id", [](const HttpRequest& req) -> HttpResponse {
		try {
			auto params = ParseQuery(req.Query);
			int id = std::stoi(params["id"]);
			json body = json::parse(req.Body);
			bool enabled = body.value("enabled", true);

			std::lock_guard<std::mutex> lk(g_HookMutex);
			auto it = g_Hooks.find(id);
			if (it == g_Hooks.end())
				return { 404, "application/json", MakeError("Hook not found") };

			it->second.Enabled = enabled;

			json data;
			data["id"] = id;
			data["enabled"] = enabled;
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 400, "application/json", MakeError("Invalid request") };
		}
	});

	// GET /api/v1/hooks/list — list all hooks
	server.Get("/api/v1/hooks/list", [](const HttpRequest&) -> HttpResponse {
		json data;
		json hooks = json::array();

		std::lock_guard<std::mutex> lk(g_HookMutex);
		for (const auto& [id, h] : g_Hooks)
		{
			json hook;
			hook["id"] = h.Id;
			hook["function_path"] = h.FunctionPath;
			hook["enabled"] = h.Enabled;
			hook["hit_count"] = h.HitCount;
			hook["last_hit_time"] = h.LastHitTime;
			hooks.push_back(hook);
		}
		data["hooks"] = hooks;
		data["monitored_count"] = g_MonitoredFunctions.size();
		data["total_pe_calls"] = g_TotalPECalls.load();
		data["vtable_hook_installed"] = g_VTableHookInstalled;
		data["game_thread_enabled"] = GameThread::g_Enabled;

		return { 200, "application/json", MakeResponse(data) };
	});

	// GET /api/v1/hooks/:id/log — get hook log
	server.Get("/api/v1/hooks/:id/log", [](const HttpRequest& req) -> HttpResponse {
		try {
			auto params = ParseQuery(req.Query);
			int id = std::stoi(params["id"]);

			json entries = json::array();
			int64_t now = HookNowMs();

			std::lock_guard<std::mutex> lk(g_LogMutex);
			for (const auto& e : g_HookLog)
			{
				if (e.HookId == id || id == 0)
				{
					json entry;
					entry["timestamp"] = e.Timestamp;
					entry["function_name"] = e.FunctionName;
					entry["caller_name"] = e.CallerName;
					entries.push_back(entry);
				}
			}

			json data;
			data["entries"] = entries;
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 400, "application/json", MakeError("Invalid request") };
		}
	});
}

void InitHooks()
{
	// Install PostRender VTable hook on startup
	InstallPostRenderHook();
}

void ShutdownHooks()
{
	UninstallPostRenderHook();

	std::lock_guard<std::mutex> lk(g_HookMutex);
	g_Hooks.clear();
	g_MonitoredFunctions.clear();
}

} // namespace UExplorer::API
