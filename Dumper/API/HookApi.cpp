#include "HookApi.h"
#include "ApiCommon.h"
#include "GameThreadQueue.h"
#include "EventsApi.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/UnrealObjects.h"
#include "Unreal/UnrealTypes.h"
#include "Unreal/Enums.h"
#include "OffsetFinder/Offsets.h"
#include "Runtime/CallbackBarrier.h"
#include "Runtime/SafeMemory.h"
#include "Runtime/VTableHook.h"

#include <algorithm>
#include <format>
#include <mutex>
#include <shared_mutex>
#include <map>
#include <memory>
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
static std::unique_ptr<Runtime::VTableHookToken> g_PostRenderPatch;
static std::atomic<void*> g_OrigPostRender{nullptr};
// Read-only diagnostic projections. Patch ownership remains exclusively in tokens.
static std::atomic<bool> g_PostRenderPublishedActive{false};

// ProcessEvent vtable-slot hook state (non-inline).
static std::mutex g_PEVTableMutex;
static std::vector<std::unique_ptr<Runtime::VTableHookToken>> g_PEPatches;
static std::atomic<void*> g_OriginalPE{nullptr};
static std::atomic<bool> g_PEPublishedActive{false};
static std::atomic<int64_t> g_TotalPECalls{ 0 };
static Runtime::CallbackBarrier g_PECallbackBarrier;
static Runtime::CallbackBarrier g_PostRenderCallbackBarrier;
static std::atomic<bool> g_HookShutdownRequested{ false };
static constexpr bool kLegacyHookMonitoringEnabled = false;

typedef void(*PostRenderFn)(void*, void*);
typedef void(*ProcessEventFn)(void*, void*, void*);

static bool IsPostRenderHookInstalled()
{
	return g_PostRenderPublishedActive.load(std::memory_order_acquire);
}

static bool IsPEHookInstalled()
{
	return g_PEPublishedActive.load(std::memory_order_acquire);
}

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

static bool ReadPointerSlot(void** slot, void*& value)
{
	value = nullptr;
	return slot
		&& Runtime::ReadValue(reinterpret_cast<std::uintptr_t>(slot), value).Ok();
}

static bool ReadObjectVTable(void* object, void**& vtable)
{
	vtable = nullptr;
	return object
		&& Runtime::ReadValue(reinterpret_cast<std::uintptr_t>(object), vtable).Ok()
		&& vtable;
}

static void HookedProcessEvent(void* Object, void* Function, void* Params)
{
	auto callbackLease = g_PECallbackBarrier.Enter();
	g_TotalPECalls.fetch_add(1, std::memory_order_relaxed);

	bool monitored = false;
	if (callbackLease.OwnedWorkAllowed()
		&& !g_HookShutdownRequested.load(std::memory_order_acquire))
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

	auto orig = reinterpret_cast<ProcessEventFn>(g_OriginalPE.load(std::memory_order_acquire));
	if (orig)
	{
		orig(Object, Function, Params);
	}
}

static bool InstallPEVTableHook()
{
	std::lock_guard<std::mutex> lk(g_PEVTableMutex);
	if (!g_PEPatches.empty()
		&& std::ranges::any_of(g_PEPatches, [](const auto& patch) {
			return patch && patch->IsActive();
		}))
	{
		return true;
	}
	if (!g_PEPatches.empty())
	{
		std::cerr << "[HookApi] ProcessEvent hook owner still has an incomplete shutdown" << std::endl;
		return false;
	}
	g_PEPublishedActive.store(false, std::memory_order_release);
	if (!g_PECallbackBarrier.Reset())
	{
		std::cerr << "[HookApi] ProcessEvent callback barrier is not drained" << std::endl;
		return false;
	}

	const int32 peIdx = Off::InSDK::ProcessEvent::PEIndex;
	if (peIdx <= 0 || peIdx >= kMaxVTableIndex)
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

	void** cdoVft = nullptr;
	if (!ReadObjectVTable(uObjectCDO.GetAddress(), cdoVft))
	{
		std::cerr << "[HookApi] UObject vtable not available" << std::endl;
		return false;
	}

	void* targetPE = nullptr;
	if (!ReadPointerSlot(cdoVft + peIdx, targetPE) || !targetPE)
	{
		std::cerr << "[HookApi] UObject ProcessEvent pointer not available" << std::endl;
		return false;
	}

	g_OriginalPE.store(targetPE, std::memory_order_release);

	std::unordered_set<void**> visitedVTables;
	std::vector<void**> candidateSlots;
	int classCount = 0;
	try
	{
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

			void** vft = nullptr;
			if (!ReadObjectVTable(clsCdoAddr, vft) || !visitedVTables.insert(vft).second)
			{
				continue;
			}

			void** slot = vft + peIdx;
			void* current = nullptr;
			if (!ReadPointerSlot(slot, current) || current != targetPE)
			{
				continue;
			}
			candidateSlots.push_back(slot);
		}

		// No vector growth may occur after a slot has been patched.
		g_PEPatches.reserve(candidateSlots.size());
	}
	catch (...)
	{
		g_OriginalPE.store(nullptr, std::memory_order_release);
		g_PEPublishedActive.store(false, std::memory_order_release);
		std::cerr << "[HookApi] Failed to enumerate ProcessEvent patch candidates" << std::endl;
		return false;
	}

	int patched = 0;
	for (void** slot : candidateSlots)
	{
		Runtime::VTableHookInstallResult installed = Runtime::VTableHookToken::Install(
			slot,
			reinterpret_cast<void*>(&HookedProcessEvent),
			targetPE);
		if (!installed.Ok())
		{
			continue;
		}

		g_PEPatches.push_back(std::move(installed.Token));
		++patched;
	}

	if (patched <= 0)
	{
		g_PEPatches.clear();
		g_OriginalPE.store(nullptr, std::memory_order_release);
		g_PEPublishedActive.store(false, std::memory_order_release);
		std::cerr << "[HookApi] Failed to patch any ProcessEvent vtable slots" << std::endl;
		return false;
	}
	g_PEPublishedActive.store(true, std::memory_order_release);

	std::cerr << "[HookApi] ProcessEvent vtable hook installed: patched_slots=" << patched
		<< " scanned_classes=" << classCount << " pe_index=" << peIdx << std::endl;
	return true;
}

static bool UninstallPEVTableHook()
{
	std::lock_guard<std::mutex> lk(g_PEVTableMutex);
	if (g_PEPatches.empty())
	{
		g_PEPublishedActive.store(false, std::memory_order_release);
		g_OriginalPE.store(nullptr, std::memory_order_release);
		return true;
	}
	g_PECallbackBarrier.BeginStopping();

	int restored = 0;
	int failed = 0;
	for (auto it = g_PEPatches.rbegin(); it != g_PEPatches.rend(); ++it)
	{
		if (!*it || !(*it)->IsActive())
		{
			continue;
		}
		if ((*it)->Disable())
		{
			++restored;
		}
		else
		{
			++failed;
		}
	}
	const bool anyActive = std::ranges::any_of(g_PEPatches, [](const auto& patch) {
		return patch && patch->IsActive();
	});
	g_PEPublishedActive.store(anyActive, std::memory_order_release);
	if (failed > 0)
	{
		std::cerr << "[HookApi] ProcessEvent hook restore failed: restored_slots=" << restored
			<< " failed_checks=" << failed << std::endl;
		return false;
	}
	if (!g_PECallbackBarrier.WaitForDrain(std::chrono::milliseconds(5000)))
	{
		std::cerr << "[HookApi] ProcessEvent callback drain timed out: in_flight="
			<< g_PECallbackBarrier.InFlight() << std::endl;
		return false;
	}

	g_PEPatches.clear();
	g_OriginalPE.store(nullptr, std::memory_order_release);
	g_PEPublishedActive.store(false, std::memory_order_release);
	std::cerr << "[HookApi] ProcessEvent vtable hook uninstalled: restored_slots=" << restored << std::endl;
	return true;
}

static void HookedPostRender(void* InGVCCDO, void* InCanvas)
{
	auto callbackLease = g_PostRenderCallbackBarrier.Enter();
	if (callbackLease.OwnedWorkAllowed()
		&& !g_HookShutdownRequested.load(std::memory_order_acquire))
		GameThread::ProcessQueue();

	auto orig = reinterpret_cast<PostRenderFn>(g_OrigPostRender.load(std::memory_order_acquire));
	if (orig)
	{
		orig(InGVCCDO, InCanvas);
	}
}

static bool DetachPostRenderPatch(const char* failureContext)
{
	if (!g_PostRenderPatch)
	{
		g_PostRenderPublishedActive.store(false, std::memory_order_release);
		return true;
	}
	g_PostRenderCallbackBarrier.BeginStopping();
	if (g_PostRenderPatch->IsActive() && !g_PostRenderPatch->Disable())
	{
		std::cerr << "[HookApi] " << failureContext
			<< ": VTable restore failed; unload is unsafe" << std::endl;
		return false;
	}
	g_PostRenderPublishedActive.store(false, std::memory_order_release);
	if (!g_PostRenderCallbackBarrier.WaitForDrain(std::chrono::milliseconds(5000)))
	{
		std::cerr << "[HookApi] " << failureContext << ": callback drain timed out: in_flight="
			<< g_PostRenderCallbackBarrier.InFlight() << std::endl;
		return false;
	}
	g_PostRenderPatch.reset();
	g_OrigPostRender.store(nullptr, std::memory_order_release);
	return true;
}

static bool InstallPostRenderHook()
{
	if (g_PostRenderPatch && g_PostRenderPatch->IsActive())
	{
		return true;
	}
	if (g_PostRenderPatch)
	{
		std::cerr << "[HookApi] PostRender hook owner still has an incomplete shutdown" << std::endl;
		return false;
	}
	g_PostRenderPublishedActive.store(false, std::memory_order_release);
	if (!g_PostRenderCallbackBarrier.Reset())
	{
		std::cerr << "[HookApi] PostRender callback barrier is not drained" << std::endl;
		return false;
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

	void* gvcPtr = cdo.GetAddress();
	if (!gvcPtr)
	{
		std::cerr << "[HookApi] Invalid GVC pointer" << std::endl;
		return false;
	}

	void** gvcVft = nullptr;
	if (!ReadObjectVTable(gvcPtr, gvcVft))
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

	void* originalPostRender = nullptr;
	void** postRenderSlot = gvcVft + postRenderIdx;
	if (!ReadPointerSlot(postRenderSlot, originalPostRender) || !originalPostRender)
	{
		std::cerr << "[HookApi] Original PostRender not found" << std::endl;
		return false;
	}

	g_OrigPostRender.store(originalPostRender, std::memory_order_release);
	Runtime::VTableHookInstallResult installed = Runtime::VTableHookToken::Install(
		postRenderSlot,
		reinterpret_cast<void*>(&HookedPostRender),
		originalPostRender);
	if (!installed.Ok())
	{
		g_OrigPostRender.store(nullptr, std::memory_order_release);
		std::cerr << "[HookApi] Failed to patch PostRender vtable slot" << std::endl;
		return false;
	}
	g_PostRenderPatch = std::move(installed.Token);
	g_PostRenderPublishedActive.store(true, std::memory_order_release);

	void* globalPE = g_OriginalPE.load(std::memory_order_acquire);
	if (!globalPE)
	{
		UEClass uObjectClass = ObjectArray::FindClassFast("Object");
		if (uObjectClass)
		{
			UEObject uObjectCDO = uObjectClass.GetDefaultObject();
			if (uObjectCDO)
			{
				void** cdoVft = nullptr;
				const int32 peIdx = Off::InSDK::ProcessEvent::PEIndex;
				if (peIdx > 0 && peIdx < 512
					&& ReadObjectVTable(uObjectCDO.GetAddress(), cdoVft))
				{
					ReadPointerSlot(cdoVft + peIdx, globalPE);
				}
			}
		}
	}

	if (globalPE)
	{
		std::cerr << "[HookApi] Global ProcessEvent: " << std::hex << globalPE << std::dec << std::endl;
		if (!GameThread::Enable(reinterpret_cast<ProcessEventFn>(globalPE)))
		{
			if (!DetachPostRenderPatch("executor enable rollback"))
			{
				return false;
			}
			std::cerr << "[HookApi] Failed to enable game-thread executor" << std::endl;
			return false;
		}
	}
	else
	{
		if (!DetachPostRenderPatch("missing ProcessEvent rollback"))
		{
			return false;
		}
		std::cerr << "[HookApi] Global ProcessEvent unavailable; game-thread executor disabled" << std::endl;
		return false;
	}

	std::cerr << "[HookApi] PostRender vtable hook installed: index=" << postRenderIdx << std::endl;
	return true;
}

static bool UninstallPostRenderHook()
{
	if (!g_PostRenderPatch)
	{
		g_PostRenderPublishedActive.store(false, std::memory_order_release);
		return true;
	}

	std::cerr << "[HookApi] Uninstalling PostRender hook..." << std::endl;
	g_PostRenderCallbackBarrier.BeginStopping();
	if (!GameThread::DisableAndDrain(5000))
	{
		std::cerr << "[HookApi] Game-thread executor drain timed out" << std::endl;
		return false;
	}

	if (!DetachPostRenderPatch("PostRender shutdown"))
		return false;

	std::cerr << "[HookApi] PostRender hook uninstalled" << std::endl;
	return true;
}

static bool InstallPEHook()
{
	return InstallPEVTableHook();
}

static bool UninstallPEHook()
{
	return UninstallPEVTableHook();
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
		if (!kLegacyHookMonitoringEnabled)
			return { 409, "application/json", MakeError("HOOK_MONITORING_UNAVAILABLE: bounded non-blocking collector is not active") };
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
		if (!kLegacyHookMonitoringEnabled)
			return { 409, "application/json", MakeError("HOOK_MONITORING_UNAVAILABLE: bounded non-blocking collector is not active") };
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
		if (!kLegacyHookMonitoringEnabled)
			return { 409, "application/json", MakeError("HOOK_MONITORING_UNAVAILABLE: bounded non-blocking collector is not active") };
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

	server.Get("/api/v1/hooks/list", [](const HttpRequest& req) -> HttpResponse {
		if (!kLegacyHookMonitoringEnabled)
		{
			json data;
			data["hooks"] = json::array();
			data["available"] = false;
			data["reason"] = "BOUNDED_COLLECTOR_NOT_ACTIVE";
			data["postrender_vtable_hook_installed"] = IsPostRenderHookInstalled();
			data["pe_vtable_hook_installed"] = false;
			data["game_thread_enabled"] = GameThread::IsEnabled();
			return { 200, "application/json", MakeResponse(data) };
		}
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
		data["vtable_hook_installed"] = IsPEHookInstalled() || IsPostRenderHookInstalled();
		data["postrender_vtable_hook_installed"] = IsPostRenderHookInstalled();
		data["pe_vtable_hook_installed"] = IsPEHookInstalled();
		data["game_thread_enabled"] = GameThread::IsEnabled();

		return { 200, "application/json", MakeResponse(data) };
	});

	server.Get("/api/v1/hooks/:id/log", [](const HttpRequest& req) -> HttpResponse {
		if (!kLegacyHookMonitoringEnabled)
			return { 409, "application/json", MakeError("HOOK_MONITORING_UNAVAILABLE: bounded non-blocking collector is not active") };
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

bool InitHooks()
{
	g_HookShutdownRequested.store(false, std::memory_order_release);
	// ProcessEvent monitoring is installed lazily when the first subscription is added.
	return InstallPostRenderHook();
}

bool ShutdownHooks()
{
	g_HookShutdownRequested.store(true, std::memory_order_release);
	const bool postRenderStopped = UninstallPostRenderHook();
	const bool processEventStopped = UninstallPEHook();
	if (!postRenderStopped || !processEventStopped)
		return false;

	std::lock_guard<std::mutex> lk(g_HookMutex);
	g_Hooks.clear();

	std::unique_lock<std::shared_mutex> mlk(g_MonitorMutex);
	g_MonitoredFunctions.clear();
	return true;
}

} // namespace UExplorer::API
