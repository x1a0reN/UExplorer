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
#include <shared_mutex>
#include <map>
#include <set>
#include <atomic>
#include <vector>
#include <deque>
#include <chrono>
#include <unordered_set>
#include <cctype>

namespace UExplorer::API
{

struct HookEntry
{
	int Id;
	std::string FunctionPath;
	std::vector<void*> FunctionAddresses;
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

struct PatchedPESlot
{
	void** Slot = nullptr;
	void* Original = nullptr;
};

static std::mutex g_HookMutex;
static std::map<int, HookEntry> g_Hooks;
static std::atomic<int> g_HookCounter{ 0 };

static std::mutex g_LogMutex;
static std::deque<HookLogEntry> g_HookLog;
static constexpr size_t MAX_LOG_SIZE = 2000;

// Set of monitored UFunction addresses for fast lookup in hot path.
// Uses shared_mutex: HookedProcessEvent takes shared (read) lock, add/remove take unique (write) lock.
static std::shared_mutex g_MonitorMutex;
static std::set<void*> g_MonitoredFunctions;

// PostRender vtable hook is only used for game-thread dispatch queue.
static void* g_GVCPtr = nullptr;
static void** g_GVCVft = nullptr;
static void* g_OrigPostRender = nullptr;
static bool g_PostRenderHookInstalled = false;

// ProcessEvent vtable-slot hook state (non-inline).
static std::mutex g_PEVTableMutex;
static std::vector<PatchedPESlot> g_PatchedPESlots;
static void* g_OriginalPE = nullptr;
static bool g_PEHookInstalled = false;
static std::atomic<int64_t> g_TotalPECalls{ 0 };

typedef void(*PostRenderFn)(void*, void*);
typedef void(*ProcessEventFn)(void*, void*, void*);

static int64_t HookNowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string Trim(const std::string& input)
{
	size_t begin = 0;
	while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])))
	{
		++begin;
	}

	size_t end = input.size();
	while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])))
	{
		--end;
	}

	return input.substr(begin, end - begin);
}

static bool ParseFunctionPath(const std::string& rawPath, std::string& outClassName, std::string& outFuncName)
{
	std::string path = Trim(rawPath);
	if (path.empty())
	{
		return false;
	}

	const std::string functionPrefix = "Function ";
	if (path.rfind(functionPrefix, 0) == 0)
	{
		path = Trim(path.substr(functionPrefix.size()));
	}

	size_t lastDot = path.rfind('.');
	if (lastDot == std::string::npos || lastDot == 0 || lastDot + 1 >= path.size())
	{
		return false;
	}

	outFuncName = Trim(path.substr(lastDot + 1));
	std::string left = path.substr(0, lastDot);
	size_t classDot = left.rfind('.');
	outClassName = Trim(classDot == std::string::npos ? left : left.substr(classDot + 1));

	return !outClassName.empty() && !outFuncName.empty();
}

static UEClass FindClassByName(const std::string& className)
{
	for (int i = 0; i < ObjectArray::Num(); ++i)
	{
		UEObject obj = ObjectArray::GetByIndex(i);
		if (!obj || !obj.IsA(EClassCastFlags::Class))
		{
			continue;
		}

		if (obj.GetName() == className)
		{
			return obj.Cast<UEClass>();
		}
	}

	return {};
}

static UEFunction FindFunctionInClassHierarchy(UEClass cls, const std::string& funcName)
{
	for (UEStruct current = cls; current; current = current.GetSuper())
	{
		for (UEField field = current.GetChild(); field; field = field.GetNext())
		{
			if (field.IsA(EClassCastFlags::Function) && field.GetName() == funcName)
			{
				return field.Cast<UEFunction>();
			}
		}
	}

	return {};
}

static bool ResolveFunctionAddress(const std::string& functionPath, std::string& outClassName, std::string& outFuncName, void*& outFuncAddr)
{
	if (!ParseFunctionPath(functionPath, outClassName, outFuncName))
	{
		return false;
	}

	UEClass cls = FindClassByName(outClassName);
	if (!cls)
	{
		return false;
	}

	UEFunction func = FindFunctionInClassHierarchy(cls, outFuncName);
	if (!func)
	{
		return false;
	}

	outFuncAddr = func.GetAddress();
	return outFuncAddr != nullptr;
}

static bool PatchVTableSlot(void** slot, void* replacement, void** outOriginal = nullptr)
{
	if (!slot)
	{
		return false;
	}

	DWORD oldProtect = 0;
	if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
	{
		return false;
	}

	if (outOriginal)
	{
		*outOriginal = *slot;
	}
	*slot = replacement;

	DWORD dummy = 0;
	VirtualProtect(slot, sizeof(void*), oldProtect, &dummy);
	return true;
}

static bool RestoreVTableSlot(void** slot, void* original)
{
	if (!slot)
	{
		return false;
	}

	DWORD oldProtect = 0;
	if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
	{
		return false;
	}

	*slot = original;
	DWORD dummy = 0;
	VirtualProtect(slot, sizeof(void*), oldProtect, &dummy);
	return true;
}

static void HookedProcessEvent(void* Object, void* Function, void* Params)
{
	g_TotalPECalls.fetch_add(1, std::memory_order_relaxed);

	bool monitored = false;
	{
		std::shared_lock<std::shared_mutex> lk(g_MonitorMutex);
		monitored = g_MonitoredFunctions.count(Function) > 0;
	}

	if (monitored)
	{
		std::string funcName;
		std::string callerName;
		try
		{
			UEFunction f(static_cast<uint8_t*>(Function));
			funcName = f.GetName();
		}
		catch (...) {}

		const int64_t now = HookNowMs();
		int matchedHookId = 0;
		{
			std::lock_guard<std::mutex> hlk(g_HookMutex);
			for (auto& [id, h] : g_Hooks)
			{
				if (!h.Enabled)
				{
					continue;
				}

				for (void* fa : h.FunctionAddresses)
				{
					if (fa == Function)
					{
						h.HitCount++;
						h.LastHitTime = now;
						if (matchedHookId == 0)
						{
							matchedHookId = id;
						}
						break;
					}
				}
			}
		}

		{
			std::lock_guard<std::mutex> llk(g_LogMutex);
			g_HookLog.push_back({ matchedHookId, funcName, callerName, now });
			if (g_HookLog.size() > MAX_LOG_SIZE)
			{
				g_HookLog.pop_front();
			}
		}

		try
		{
			json evt;
			evt["hook_id"] = matchedHookId;
			evt["function"] = funcName;
			evt["timestamp"] = now;
			BroadcastHookEvent(funcName, evt.dump());
		}
		catch (...) {}
	}

	auto orig = reinterpret_cast<ProcessEventFn>(g_OriginalPE);
	if (orig)
	{
		orig(Object, Function, Params);
	}
}

static bool InstallPEVTableHook()
{
	std::lock_guard<std::mutex> lk(g_PEVTableMutex);
	if (g_PEHookInstalled)
	{
		return true;
	}

	const int32 peIdx = Off::InSDK::ProcessEvent::PEIndex;
	if (peIdx <= 0 || peIdx >= 512)
	{
		std::cerr << "[HookApi] Invalid ProcessEvent index: " << peIdx << std::endl;
		return false;
	}

	UEClass uObjectClass = ObjectArray::FindClassFast("Object");
	if (!uObjectClass)
	{
		std::cerr << "[HookApi] UObject class not found" << std::endl;
		return false;
	}

	UEObject uObjectCDO = uObjectClass.GetDefaultObject();
	if (!uObjectCDO)
	{
		std::cerr << "[HookApi] UObject CDO not found" << std::endl;
		return false;
	}

	void* cdoAddr = uObjectCDO.GetAddress();
	void** cdoVft = cdoAddr ? *reinterpret_cast<void***>(cdoAddr) : nullptr;
	if (!cdoVft)
	{
		std::cerr << "[HookApi] UObject vtable not available" << std::endl;
		return false;
	}

	void* targetPE = cdoVft[peIdx];
	if (!targetPE)
	{
		std::cerr << "[HookApi] UObject ProcessEvent pointer not available" << std::endl;
		return false;
	}

	g_OriginalPE = targetPE;
	g_PatchedPESlots.clear();

	std::unordered_set<void**> visitedVTables;
	int classCount = 0;
	int patched = 0;

	for (int i = 0; i < ObjectArray::Num(); ++i)
	{
		UEObject obj = ObjectArray::GetByIndex(i);
		if (!obj || !obj.IsA(EClassCastFlags::Class))
		{
			continue;
		}

		++classCount;
		UEClass cls = obj.Cast<UEClass>();
		UEObject cdo = cls.GetDefaultObject();
		if (!cdo)
		{
			continue;
		}

		void* clsCdoAddr = cdo.GetAddress();
		if (!clsCdoAddr)
		{
			continue;
		}

		void** vft = *reinterpret_cast<void***>(clsCdoAddr);
		if (!vft || !visitedVTables.insert(vft).second)
		{
			continue;
		}

		void** slot = vft + peIdx;
		if (*slot != targetPE)
		{
			continue;
		}

		void* oldFunc = nullptr;
		if (!PatchVTableSlot(slot, reinterpret_cast<void*>(&HookedProcessEvent), &oldFunc))
		{
			continue;
		}

		g_PatchedPESlots.push_back({ slot, oldFunc });
		++patched;
	}

	if (patched <= 0)
	{
		g_PatchedPESlots.clear();
		g_OriginalPE = nullptr;
		std::cerr << "[HookApi] Failed to patch any ProcessEvent vtable slots" << std::endl;
		return false;
	}

	g_PEHookInstalled = true;
	std::cerr << "[HookApi] ProcessEvent vtable hook installed: patched_slots=" << patched
		<< " scanned_classes=" << classCount << " pe_index=" << peIdx << std::endl;
	return true;
}

static void UninstallPEVTableHook()
{
	std::lock_guard<std::mutex> lk(g_PEVTableMutex);
	if (!g_PEHookInstalled)
	{
		return;
	}

	int restored = 0;
	for (auto it = g_PatchedPESlots.rbegin(); it != g_PatchedPESlots.rend(); ++it)
	{
		if (!it->Slot)
		{
			continue;
		}

		if (*it->Slot == reinterpret_cast<void*>(&HookedProcessEvent))
		{
			if (RestoreVTableSlot(it->Slot, it->Original))
			{
				++restored;
			}
		}
	}

	g_PatchedPESlots.clear();
	g_PEHookInstalled = false;
	g_OriginalPE = nullptr;
	std::cerr << "[HookApi] ProcessEvent vtable hook uninstalled: restored_slots=" << restored << std::endl;
}

static void HookedPostRender(void* InGVCCDO, void* InCanvas)
{
	GameThread::ProcessQueue();

	auto orig = reinterpret_cast<PostRenderFn>(g_OrigPostRender);
	if (orig)
	{
		orig(InGVCCDO, InCanvas);
	}
}

static bool InstallPostRenderHook()
{
	if (g_PostRenderHookInstalled)
	{
		return true;
	}

	UEClass gvcClass = FindClassByName("GameViewportClient");
	if (!gvcClass)
	{
		std::cerr << "[HookApi] GameViewportClient class not found" << std::endl;
		return false;
	}

	UEObject cdo = gvcClass.GetDefaultObject();
	if (!cdo)
	{
		std::cerr << "[HookApi] GameViewportClient CDO not found" << std::endl;
		return false;
	}

	g_GVCPtr = cdo.GetAddress();
	if (!g_GVCPtr)
	{
		std::cerr << "[HookApi] Invalid GVC pointer" << std::endl;
		return false;
	}

	g_GVCVft = *reinterpret_cast<void***>(g_GVCPtr);
	if (!g_GVCVft)
	{
		std::cerr << "[HookApi] Invalid GVC vtable" << std::endl;
		return false;
	}

	const int32 postRenderIdx = Off::InSDK::PostRender::GVCPostRenderIndex;
	if (postRenderIdx < 0)
	{
		std::cerr << "[HookApi] PostRender index not found" << std::endl;
		return false;
	}

	g_OrigPostRender = g_GVCVft[postRenderIdx];
	if (!g_OrigPostRender)
	{
		std::cerr << "[HookApi] Original PostRender not found" << std::endl;
		return false;
	}

	if (!PatchVTableSlot(g_GVCVft + postRenderIdx, reinterpret_cast<void*>(&HookedPostRender)))
	{
		std::cerr << "[HookApi] Failed to patch PostRender vtable slot" << std::endl;
		return false;
	}

	g_PostRenderHookInstalled = true;

	void* globalPE = g_OriginalPE;
	if (!globalPE)
	{
		UEClass uObjectClass = ObjectArray::FindClassFast("Object");
		if (uObjectClass)
		{
			UEObject uObjectCDO = uObjectClass.GetDefaultObject();
			if (uObjectCDO)
			{
				void* cdoAddr = uObjectCDO.GetAddress();
				void** cdoVft = cdoAddr ? *reinterpret_cast<void***>(cdoAddr) : nullptr;
				const int32 peIdx = Off::InSDK::ProcessEvent::PEIndex;
				if (peIdx > 0 && peIdx < 512 && cdoVft)
				{
					globalPE = cdoVft[peIdx];
				}
			}
		}
	}

	if (globalPE)
	{
		std::cerr << "[HookApi] Global ProcessEvent: " << std::hex << globalPE << std::dec << std::endl;
		GameThread::Enable(reinterpret_cast<ProcessEventFn>(globalPE));
	}
	else
	{
		std::cerr << "[HookApi] Warning: Could not get global ProcessEvent" << std::endl;
		GameThread::Enable([](void*, void*, void*) {});
	}

	std::cerr << "[HookApi] PostRender vtable hook installed: index=" << postRenderIdx << std::endl;
	return true;
}

static void UninstallPostRenderHook()
{
	if (!g_PostRenderHookInstalled)
	{
		return;
	}

	std::cerr << "[HookApi] Uninstalling PostRender hook..." << std::endl;
	GameThread::Disable();

	const int32 postRenderIdx = Off::InSDK::PostRender::GVCPostRenderIndex;
	if (postRenderIdx >= 0 && g_GVCVft && g_OrigPostRender)
	{
		RestoreVTableSlot(g_GVCVft + postRenderIdx, g_OrigPostRender);
	}

	g_PostRenderHookInstalled = false;
	g_GVCPtr = nullptr;
	g_GVCVft = nullptr;
	g_OrigPostRender = nullptr;

	std::cerr << "[HookApi] PostRender hook uninstalled" << std::endl;
}

static bool InstallPEHook()
{
	return InstallPEVTableHook();
}

static void UninstallPEHook()
{
	UninstallPEVTableHook();
}

static bool InstallHook(const std::string& functionPath, std::string& outNormalizedPath, void*& outFuncAddr)
{
	std::string className;
	std::string funcName;
	if (!ResolveFunctionAddress(functionPath, className, funcName, outFuncAddr))
	{
		return false;
	}

	outNormalizedPath = className + "." + funcName;

	{
		std::unique_lock<std::shared_mutex> lk(g_MonitorMutex);
		g_MonitoredFunctions.insert(outFuncAddr);
	}

	return true;
}

static void UninstallHook(int id)
{
	std::lock_guard<std::mutex> lk(g_HookMutex);
	auto it = g_Hooks.find(id);
	if (it == g_Hooks.end())
	{
		return;
	}

	const std::vector<void*> removeAddresses = it->second.FunctionAddresses;
	g_Hooks.erase(it);

	std::unique_lock<std::shared_mutex> mlk(g_MonitorMutex);
	for (void* addr : removeAddresses)
	{
		bool stillUsed = false;
		for (const auto& [otherId, otherHook] : g_Hooks)
		{
			for (void* otherAddr : otherHook.FunctionAddresses)
			{
				if (otherAddr == addr)
				{
					stillUsed = true;
					break;
				}
			}
			if (stillUsed)
			{
				break;
			}
		}

		if (!stillUsed)
		{
			g_MonitoredFunctions.erase(addr);
		}
	}
}

static bool AddHook(const std::string& functionPath, int& outId)
{
	if (!InstallPEHook())
	{
		return false;
	}

	void* funcAddr = nullptr;
	std::string normalizedPath;
	if (!InstallHook(functionPath, normalizedPath, funcAddr))
	{
		return false;
	}

	std::lock_guard<std::mutex> lk(g_HookMutex);
	const int id = ++g_HookCounter;

	HookEntry entry;
	entry.Id = id;
	entry.FunctionPath = normalizedPath;
	entry.FunctionAddresses.push_back(funcAddr);
	entry.Enabled = true;
	entry.HitCount = 0;
	entry.LastHitTime = 0;
	entry.CreatedTime = HookNowMs();

	g_Hooks[id] = entry;
	outId = id;
	return true;
}

static bool TryParseIntStrict(const std::string& text, int& outValue)
{
	if (text.empty())
	{
		return false;
	}

	try
	{
		size_t parsed = 0;
		int value = std::stoi(text, &parsed);
		if (parsed != text.size())
		{
			return false;
		}

		outValue = value;
		return true;
	}
	catch (...)
	{
		return false;
	}
}

static bool ResolveHookIdFromRequest(const HttpRequest& req, int& outId)
{
	// Primary source: REST path parameter /api/v1/hooks/:id
	const std::string idFromPath = GetPathSegment(req.Path, 3);
	if (TryParseIntStrict(idFromPath, outId))
	{
		return true;
	}

	// Backward compatibility: legacy clients still pass ?id=...
	const auto params = ParseQuery(req.Query);
	const auto it = params.find("id");
	if (it != params.end() && TryParseIntStrict(it->second, outId))
	{
		return true;
	}

	return false;
}

void RegisterHookRoutes(HttpServer& server)
{
	server.Post("/api/v1/hooks/add", [](const HttpRequest& req) -> HttpResponse {
		try {
			json body = json::parse(req.Body);
			std::string functionPath = body.value("function_path", "");

			if (functionPath.empty())
				return { 400, "application/json", MakeError("Missing function_path") };

			int hookId = 0;
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

	server.Delete("/api/v1/hooks/:id", [](const HttpRequest& req) -> HttpResponse {
		try {
			int id = 0;
			if (!ResolveHookIdFromRequest(req, id))
				return { 400, "application/json", MakeError("Invalid hook id") };

			UninstallHook(id);

			json data;
			data["removed"] = true;
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 400, "application/json", MakeError("Invalid hook id") };
		}
	});

	server.Patch("/api/v1/hooks/:id", [](const HttpRequest& req) -> HttpResponse {
		try {
			int id = 0;
			if (!ResolveHookIdFromRequest(req, id))
				return { 400, "application/json", MakeError("Invalid hook id") };
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
		{
			std::shared_lock<std::shared_mutex> mlk(g_MonitorMutex);
			data["monitored_count"] = g_MonitoredFunctions.size();
		}
		data["total_pe_calls"] = g_TotalPECalls.load();
		data["vtable_hook_installed"] = g_PostRenderHookInstalled;
		data["postrender_vtable_hook_installed"] = g_PostRenderHookInstalled;
		data["pe_vtable_hook_installed"] = g_PEHookInstalled;
		data["game_thread_enabled"] = GameThread::g_Enabled;

		return { 200, "application/json", MakeResponse(data) };
	});

	server.Get("/api/v1/hooks/:id/log", [](const HttpRequest& req) -> HttpResponse {
		try {
			int id = 0;
			if (!ResolveHookIdFromRequest(req, id))
				return { 400, "application/json", MakeError("Invalid hook id") };

			json entries = json::array();

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
	// 1) install ProcessEvent vtable hook for monitoring
	InstallPEHook();
	// 2) install PostRender vtable hook for game-thread dispatch
	InstallPostRenderHook();
}

void ShutdownHooks()
{
	UninstallPostRenderHook();
	UninstallPEHook();

	std::lock_guard<std::mutex> lk(g_HookMutex);
	g_Hooks.clear();

	std::unique_lock<std::shared_mutex> mlk(g_MonitorMutex);
	g_MonitoredFunctions.clear();
}

} // namespace UExplorer::API
