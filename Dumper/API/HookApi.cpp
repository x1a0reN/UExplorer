#include "WinMemApi.h"

#include "HookApi.h"
#include "ApiCommon.h"
#include "GameThreadQueue.h"

#include "Unreal/ObjectArray.h"
#include "Unreal/UnrealObjects.h"
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

// ── Inline detour infrastructure ─────────────────────────────
// Patch the actual ProcessEvent function body with a JMP to our hook.
// This intercepts ALL calls regardless of VTable state.
typedef void (*ProcessEventFn)(void*, void*, void*);
static ProcessEventFn g_OriginalPE = nullptr;           // trampoline that calls original code
static void* g_OriginalPEAddr = nullptr;                // address of the real ProcessEvent
static uint8_t g_SavedBytes[32] = {};                   // original bytes we overwrote
static int g_SavedBytesLen = 0;                         // how many bytes we saved
static void* g_Trampoline = nullptr;                    // allocated trampoline memory
static bool g_HookInstalled = false;
static std::atomic<int64_t> g_TotalPECalls{0};          // debug: count ALL ProcessEvent calls

static int64_t HookNowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

// ── Minimal x64 instruction length decoder ──────────────────
// Handles common function prologue instructions. Returns 0 on failure.
static int GetInstructionLength(const uint8_t* code)
{
	const uint8_t* p = code;
	bool hasRex = (*p >= 0x40 && *p <= 0x4F);
	if (hasRex) p++;

	uint8_t op = *p++;
	switch (op)
	{
	case 0x50: case 0x51: case 0x52: case 0x53: // push r64
	case 0x54: case 0x55: case 0x56: case 0x57:
	case 0x58: case 0x59: case 0x5A: case 0x5B: // pop r64
	case 0x5C: case 0x5D: case 0x5E: case 0x5F:
	case 0x90: case 0xCC: case 0xC3:             // nop, int3, ret
		return (int)(p - code);

	case 0x89: case 0x8B: case 0x8D: // mov r/m, lea
	case 0x31: case 0x33: case 0x85: // xor, test
	case 0x3B: case 0x39: case 0x29: case 0x01: // cmp, sub, add
	{
		uint8_t modrm = *p++;
		uint8_t mod = (modrm >> 6) & 3;
		uint8_t rm = modrm & 7;
		if (mod == 3) return (int)(p - code);
		if (mod == 0 && rm == 5) return (int)(p - code) + 4;    // [rip+disp32]
		if (rm == 4) p++; // SIB byte
		if (mod == 0) return (int)(p - code);
		if (mod == 1) return (int)(p - code) + 1;
		if (mod == 2) return (int)(p - code) + 4;
		return 0;
	}

	case 0x83: // op r/m, imm8
	{
		uint8_t modrm = *p++;
		uint8_t mod = (modrm >> 6) & 3;
		uint8_t rm = modrm & 7;
		if (mod == 3) return (int)(p - code) + 1;
		if (rm == 4) p++;
		if (mod == 0) return (int)(p - code) + 1;
		if (mod == 1) return (int)(p - code) + 2;
		if (mod == 2) return (int)(p - code) + 5;
		return 0;
	}
	case 0x81: // op r/m, imm32
	{
		uint8_t modrm = *p++;
		uint8_t mod = (modrm >> 6) & 3;
		uint8_t rm = modrm & 7;
		if (mod == 3) return (int)(p - code) + 4;
		if (rm == 4) p++;
		if (mod == 0) return (int)(p - code) + 4;
		if (mod == 1) return (int)(p - code) + 5;
		if (mod == 2) return (int)(p - code) + 8;
		return 0;
	}

	case 0xB8: case 0xB9: case 0xBA: case 0xBB: // mov r, imm
	case 0xBC: case 0xBD: case 0xBE: case 0xBF:
		if (hasRex && (code[0] & 0x08)) return (int)(p - code) + 8;
		return (int)(p - code) + 4;

	default:
		return 0;
	}
}

// ── Our ProcessEvent hook (x64 ABI: rcx=this, rdx=Function, r8=Params) ──
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
			UEObject funcObj(Function);
			funcName = funcObj ? funcObj.GetName() : "Unknown";
			UEObject callerObj(Object);
			callerName = callerObj ? callerObj.GetName() : "Unknown";
		} catch (...) {
			funcName = "?"; callerName = "?";
		}

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
	}

	// Process any pending game-thread dispatch calls
	GameThread::ProcessQueue();

	// Call original ProcessEvent via saved pointer
	g_OriginalPE(Object, Function, Params);
}

// ── Install inline detour on ProcessEvent ────────────────────
static bool InstallPEHook()
{
	if (g_HookInstalled) return true;
	if (Off::InSDK::ProcessEvent::PEIndex == 0) return false;

	int peIdx = Off::InSDK::ProcessEvent::PEIndex;

	// 1) Discover the original ProcessEvent address from VTable
	UEObject firstObj = ObjectArray::GetByIndex(0);
	if (!firstObj) return false;

	void** firstVft = *reinterpret_cast<void***>(firstObj.GetAddress());
	g_OriginalPEAddr = firstVft[peIdx];
	if (!g_OriginalPEAddr) return false;

	uint8_t* target = reinterpret_cast<uint8_t*>(g_OriginalPEAddr);

	// 2) Calculate how many bytes we need to overwrite (>= 14 for absolute jmp)
	constexpr int JMP_SIZE = 14; // FF 25 00 00 00 00 + 8-byte addr
	int totalLen = 0;
	while (totalLen < JMP_SIZE)
	{
		int len = GetInstructionLength(target + totalLen);
		if (len == 0)
		{
			std::cerr << "[UExplorer] Failed to decode instruction at PE+"
				<< totalLen << " (byte=0x"
				<< std::hex << (int)target[totalLen] << std::dec << ")" << std::endl;
			return false;
		}
		totalLen += len;
	}
	if (totalLen > (int)sizeof(g_SavedBytes)) return false;

	// 3) Allocate trampoline: saved bytes + 14-byte jump back
	size_t trampolineSize = totalLen + JMP_SIZE;
	g_Trampoline = VirtualAlloc(nullptr, trampolineSize,
		MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!g_Trampoline) return false;

	uint8_t* tramp = reinterpret_cast<uint8_t*>(g_Trampoline);

	// Copy original prologue bytes to trampoline
	std::memcpy(tramp, target, totalLen);

	// Append absolute jump back to original+totalLen
	uint8_t* jumpBack = tramp + totalLen;
	jumpBack[0] = 0xFF; jumpBack[1] = 0x25;
	jumpBack[2] = 0x00; jumpBack[3] = 0x00;
	jumpBack[4] = 0x00; jumpBack[5] = 0x00;
	uint64_t returnAddr = reinterpret_cast<uint64_t>(target) + totalLen;
	std::memcpy(jumpBack + 6, &returnAddr, 8);

	g_OriginalPE = reinterpret_cast<ProcessEventFn>(g_Trampoline);
	g_SavedBytesLen = totalLen;
	std::memcpy(g_SavedBytes, target, totalLen);

	// 4) Overwrite original function start with jump to our hook
	DWORD oldProtect;
	if (!VirtualProtect(target, totalLen, PAGE_EXECUTE_READWRITE, &oldProtect))
	{
		VirtualFree(g_Trampoline, 0, MEM_RELEASE);
		g_Trampoline = nullptr;
		return false;
	}

	// Write: FF 25 00 00 00 00 [8-byte hookAddr]
	target[0] = 0xFF; target[1] = 0x25;
	target[2] = 0x00; target[3] = 0x00;
	target[4] = 0x00; target[5] = 0x00;
	uint64_t hookAddr = reinterpret_cast<uint64_t>(&HookedProcessEvent);
	std::memcpy(target + 6, &hookAddr, 8);

	// NOP remaining bytes
	for (int i = JMP_SIZE; i < totalLen; i++)
		target[i] = 0x90;

	VirtualProtect(target, totalLen, oldProtect, &oldProtect);

	g_HookInstalled = true;
	GameThread::Enable(reinterpret_cast<void(*)(void*,void*,void*)>(g_OriginalPE));

	std::cerr << "[UExplorer] ProcessEvent inline detour installed (original=0x"
		<< std::hex << reinterpret_cast<uintptr_t>(g_OriginalPEAddr)
		<< ", trampoline=0x" << reinterpret_cast<uintptr_t>(g_Trampoline)
		<< ", overwritten=" << std::dec << totalLen << " bytes)" << std::endl;
	return true;
}

static void UninstallPEHook()
{
	if (!g_HookInstalled || !g_OriginalPEAddr) return;

	// Restore original bytes
	uint8_t* target = reinterpret_cast<uint8_t*>(g_OriginalPEAddr);
	DWORD oldProtect;
	if (VirtualProtect(target, g_SavedBytesLen, PAGE_EXECUTE_READWRITE, &oldProtect))
	{
		std::memcpy(target, g_SavedBytes, g_SavedBytesLen);
		VirtualProtect(target, g_SavedBytesLen, oldProtect, &oldProtect);
	}

	// Free trampoline
	if (g_Trampoline)
	{
		VirtualFree(g_Trampoline, 0, MEM_RELEASE);
		g_Trampoline = nullptr;
	}

	g_HookInstalled = false;
	g_OriginalPE = nullptr;
	g_OriginalPEAddr = nullptr;
	g_SavedBytesLen = 0;
	GameThread::Disable();
	std::cerr << "[UExplorer] ProcessEvent inline detour removed" << std::endl;
}

void RegisterHookRoutes(HttpServer& server)
{
	// POST /api/v1/hooks/add — add a function hook
	server.Post("/api/v1/hooks/add", [](const HttpRequest& req) -> HttpResponse {
		try {
			json body = json::parse(req.Body);
			std::string funcPath = body.value("function_path", "");
			if (funcPath.empty())
				return { 400, "application/json", MakeError("Missing 'function_path'") };

			// Find ALL matching UFunctions by name
			std::vector<void*> funcAddrs;
			int32 total = ObjectArray::Num();
			for (int32 i = 0; i < total; i++)
			{
				UEObject obj = ObjectArray::GetByIndex(i);
				if (!obj) continue;
				if (!obj.IsA(EClassCastFlags::Function)) continue;
				try {
					if (obj.GetName() == funcPath || obj.GetFullName().find(funcPath) != std::string::npos)
						funcAddrs.push_back(obj.GetAddress());
				} catch (...) {}
			}
			if (funcAddrs.empty())
				return { 404, "application/json", MakeError("Function not found: " + funcPath) };

			// Install PE hook if not already
			if (!g_HookInstalled && !InstallPEHook())
				return { 500, "application/json", MakeError("Failed to install ProcessEvent hook") };

			int64_t now = HookNowMs();
			int id = ++g_HookCounter;

			{
				std::lock_guard<std::mutex> lk(g_HookMutex);
				g_Hooks[id] = { id, funcPath, funcAddrs, true, 0, 0, now };
			}
			{
				std::lock_guard<std::mutex> lk(g_MonitorMutex);
				for (void* fa : funcAddrs)
					g_MonitoredFunctions.insert(fa);
			}

			json data;
			data["id"] = id;
			data["function_path"] = funcPath;
			data["matched_functions"] = (int)funcAddrs.size();
			data["hook_installed"] = g_HookInstalled;
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
			int id = std::stoi(GetPathSegment(req.Path, 3));
			std::vector<void*> removedAddrs;

			{
				std::lock_guard<std::mutex> lk(g_HookMutex);
				auto it = g_Hooks.find(id);
				if (it == g_Hooks.end())
					return { 404, "application/json", MakeError("Hook not found") };
				removedAddrs = it->second.FunctionAddresses;
				g_Hooks.erase(it);
			}

			// Remove from monitored set if no other hook uses these addresses
			{
				std::set<void*> stillUsed;
				{
					std::lock_guard<std::mutex> hlk(g_HookMutex);
					for (auto& [hid, h] : g_Hooks) {
						if (!h.Enabled) continue;
						for (void* a : h.FunctionAddresses) stillUsed.insert(a);
					}
				}
				std::lock_guard<std::mutex> mlk(g_MonitorMutex);
				for (void* a : removedAddrs) {
					if (!stillUsed.count(a)) g_MonitoredFunctions.erase(a);
				}
			}

			json data;
			data["removed"] = id;
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 400, "application/json", MakeError("Invalid hook ID") };
		}
	});

	// PATCH /api/v1/hooks/:id — enable/disable a hook
	server.Patch("/api/v1/hooks/:id", [](const HttpRequest& req) -> HttpResponse {
		try {
			int id = std::stoi(GetPathSegment(req.Path, 3));
			json body = json::parse(req.Body);
			bool enabled = body.value("enabled", true);

			std::lock_guard<std::mutex> lk(g_HookMutex);
			auto it = g_Hooks.find(id);
			if (it == g_Hooks.end())
				return { 404, "application/json", MakeError("Hook not found") };

			it->second.Enabled = enabled;

			// Update monitored set
			{
				std::lock_guard<std::mutex> mlk(g_MonitorMutex);
				if (enabled) {
					for (void* a : it->second.FunctionAddresses)
						g_MonitoredFunctions.insert(a);
				}
				else {
					std::set<void*> stillUsed;
					for (auto& [hid, h] : g_Hooks) {
						if (hid != id && h.Enabled)
							for (void* a : h.FunctionAddresses) stillUsed.insert(a);
					}
					for (void* a : it->second.FunctionAddresses) {
						if (!stillUsed.count(a)) g_MonitoredFunctions.erase(a);
					}
				}
			}

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
		try {
			std::lock_guard<std::mutex> lk(g_HookMutex);
			json items = json::array();
			for (auto& [id, h] : g_Hooks)
			{
				json item;
				item["id"] = h.Id;
				item["function_path"] = h.FunctionPath;
				item["matched_functions"] = (int)h.FunctionAddresses.size();
				item["enabled"] = h.Enabled;
				item["hit_count"] = h.HitCount;
				item["last_hit"] = h.LastHitTime;
				item["created"] = h.CreatedTime;
				items.push_back(std::move(item));
			}
			json data;
			data["hooks"] = items;
			data["count"] = (int)g_Hooks.size();
			data["hook_installed"] = g_HookInstalled;
			data["debug_total_pe_calls"] = g_TotalPECalls.load();
			if (g_OriginalPEAddr)
				data["debug_original_pe"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(g_OriginalPEAddr));
			if (g_Trampoline)
				data["debug_trampoline"] = std::format("0x{:X}", reinterpret_cast<uintptr_t>(g_Trampoline));
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to list hooks") };
		}
	});

	// GET /api/v1/hooks/:id/log — get hook call log
	server.Get("/api/v1/hooks/:id/log", [](const HttpRequest& req) -> HttpResponse {
		try {
			std::string idStr = GetPathSegment(req.Path, 3);
			auto params = ParseQuery(req.Query);
			int limit = 100;
			if (params.count("limit")) limit = std::stoi(params["limit"]);
			if (limit > 1000) limit = 1000;

			std::lock_guard<std::mutex> lk(g_LogMutex);
			json items = json::array();
			int count = 0;
			// Return most recent entries first
			for (auto it = g_HookLog.rbegin(); it != g_HookLog.rend() && count < limit; ++it, ++count)
			{
				json item;
				item["function"] = it->FunctionName;
				item["caller"] = it->CallerName;
				item["timestamp"] = it->Timestamp;
				items.push_back(std::move(item));
			}

			json data;
			data["entries"] = items;
			data["count"] = count;
			data["total"] = (int)g_HookLog.size();
			return { 200, "application/json", MakeResponse(data) };
		}
		catch (...) {
			return { 500, "application/json", MakeError("Failed to read hook log") };
		}
	});
}

bool EnsurePEHookInstalled()
{
	return InstallPEHook();
}

} // namespace UExplorer::API
