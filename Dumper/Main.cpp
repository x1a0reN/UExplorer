#include <Windows.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <random>
#include <stdexcept>
#include <vector>

#include "Generators/Generator.h"
#include "Server/HttpServer.h"
#include "API/Router.h"
#include "API/HookApi.h"
#include "API/EventsApi.h"
#include "API/DumpApi.h"
#include "Runtime/CoreCapabilities.h"
#include "Runtime/CoreRuntimeAccess.h"
#include "Runtime/CoreSession.h"
#include "Runtime/EngineContextCapture.h"
#include "Runtime/EngineFacade.h"
#include "Runtime/GameThreadExecutor.h"
#include "Runtime/ObjectArrayIdentitySource.h"
#include "Runtime/ShutdownCoordinator.h"
#include "Services/CoreCommandService.h"
#include "Services/CoreCommandServiceAccess.h"
#include "Services/CoreStatusDiagnostics.h"
#include "Settings.h"
#include "OffsetFinder/Offsets.h"

static std::atomic<bool> g_Running{ true };
static std::unique_ptr<UExplorer::HttpServer> g_Server;
static UExplorer::Runtime::CoreRuntime g_Runtime;
static UExplorer::Services::EngineCoreStatusDiagnosticsSource g_StatusDiagnostics;
static std::unique_ptr<UExplorer::Runtime::ObjectArrayIdentitySource> g_IdentitySource;
static std::unique_ptr<UExplorer::Runtime::EngineFacade> g_EngineFacade;
static std::unique_ptr<UExplorer::Services::CoreCommandService> g_CommandService;
static HMODULE g_Module = nullptr;

namespace
{
	constexpr uint16_t kDefaultApiPort = 27015;
	constexpr const char* kDefaultApiToken = "uexplorer-dev";
	constexpr const char* kAuthMode = "token_header";

	std::string Trim(std::string value)
	{
		auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
		value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
		value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
		return value;
	}

	std::string GetLocalAppDataPath()
	{
		char buffer[MAX_PATH] = {};
		const DWORD len = GetEnvironmentVariableA("LOCALAPPDATA", buffer, MAX_PATH);
		if (len > 0 && len < MAX_PATH)
			return std::string(buffer, len);
		return {};
	}

	std::string GetUExplorerConfigDir()
	{
		namespace fs = std::filesystem;
		std::string base = GetLocalAppDataPath();
		if (base.empty())
		{
			char tempBuf[MAX_PATH] = {};
			const DWORD len = GetTempPathA(MAX_PATH, tempBuf);
			base.assign(tempBuf, len > 0 ? len : 0);
		}

		if (base.empty())
			base = ".";

		fs::path dir = fs::path(base) / "UExplorer";
		std::error_code ec;
		fs::create_directories(dir, ec);
		return dir.string();
	}

	std::string GetConnectionConfigPath()
	{
		return (std::filesystem::path(GetUExplorerConfigDir()) / "connection.ini").string();
	}

	std::string GetRuntimeStatePath()
	{
		return (std::filesystem::path(GetUExplorerConfigDir()) / "runtime.ini").string();
	}

	std::string GenerateRandomToken(size_t length = 40)
	{
		static constexpr char kAlphabet[] =
			"0123456789"
			"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
			"abcdefghijklmnopqrstuvwxyz";
		static constexpr size_t kAlphabetLen = sizeof(kAlphabet) - 1;

		std::random_device rd;
		std::mt19937_64 gen(rd());
		std::uniform_int_distribution<size_t> dist(0, kAlphabetLen - 1);

		std::string token;
		token.reserve(length);
		for (size_t i = 0; i < length; i++)
		{
			token.push_back(kAlphabet[dist(gen)]);
		}
		return token;
	}

	void EnsureDefaultConnectionConfig(const std::string& path)
	{
		const bool exists = (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES);
		if (!exists)
		{
			WritePrivateProfileStringA("Connection", "PreferredPort", std::to_string(kDefaultApiPort).c_str(), path.c_str());
			WritePrivateProfileStringA("Connection", "PortMode", "fixed", path.c_str());
		}

		char tokenBuf[256] = {};
		GetPrivateProfileStringA("Connection", "Token", "", tokenBuf, static_cast<DWORD>(sizeof(tokenBuf)), path.c_str());
		std::string token = Trim(tokenBuf);
		if (token.empty() || token == kDefaultApiToken)
		{
			token = GenerateRandomToken();
			WritePrivateProfileStringA("Connection", "Token", token.c_str(), path.c_str());
			std::cerr << "[UExplorer] Connection token generated and persisted (len=" << token.size() << ")\n";
		}
	}

	void LoadConnectionConfig(uint16_t& outPort, std::string& outToken)
	{
		const std::string cfgPath = GetConnectionConfigPath();
		EnsureDefaultConnectionConfig(cfgPath);

		int32_t preferred = GetPrivateProfileIntA("Connection", "PreferredPort", kDefaultApiPort, cfgPath.c_str());
		if (preferred < 0 || preferred > 65535)
			preferred = kDefaultApiPort;

		char modeBuf[32] = {};
		GetPrivateProfileStringA("Connection", "PortMode", "fixed", modeBuf, static_cast<DWORD>(sizeof(modeBuf)), cfgPath.c_str());
		std::string mode = Trim(modeBuf);
		std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

		char tokenBuf[256] = {};
		GetPrivateProfileStringA("Connection", "Token", kDefaultApiToken, tokenBuf, static_cast<DWORD>(sizeof(tokenBuf)), cfgPath.c_str());
		std::string token = Trim(tokenBuf);
		if (token.empty())
			token = kDefaultApiToken;

		outPort = (mode == "auto") ? 0 : static_cast<uint16_t>(preferred);
		outToken = token;
	}

	void WriteRuntimeState(uint16_t port, const std::string& token, bool running)
	{
		const std::string runtimePath = GetRuntimeStatePath();
		WritePrivateProfileStringA("Runtime", "Port", std::to_string(port).c_str(), runtimePath.c_str());
		WritePrivateProfileStringA("Runtime", "Token", token.c_str(), runtimePath.c_str());
		WritePrivateProfileStringA("Runtime", "AuthMode", kAuthMode, runtimePath.c_str());
		WritePrivateProfileStringA("Runtime", "Pid", std::to_string(GetCurrentProcessId()).c_str(), runtimePath.c_str());
		WritePrivateProfileStringA("Runtime", "Running", running ? "1" : "0", runtimePath.c_str());
	}

	void RefreshRuntimeCapabilities(const bool legacyHttpListening)
	{
		const UExplorer::Runtime::CoreRuntimeSnapshot snapshot = g_Runtime.Snapshot();
		if (!snapshot.Context)
			return;

		const UExplorer::Runtime::GameThreadDiagnostics gameThread =
			UExplorer::Runtime::GetGameThreadExecutor().GetDiagnostics();
		const std::uint64_t nowMonotonicUs = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		const bool pumpActive = gameThread.LastPumpTickMonotonicUs > 0
			&& nowMonotonicUs >= gameThread.LastPumpTickMonotonicUs
			&& nowMonotonicUs - gameThread.LastPumpTickMonotonicUs <= 2'000'000;
		UExplorer::Runtime::RuntimeProbes probes;
		probes.GameThreadExecutorEnabled = gameThread.Enabled;
		probes.GameThreadPumpObserved = gameThread.PumpObserved;
		probes.GameThreadPumpThreadStable = gameThread.PumpThreadStable;
		probes.GameThreadPumpActive = pumpActive;
		probes.SafeMemoryEnabled = true;
		probes.ObjectIdentitySourceEnabled =
			g_IdentitySource && g_IdentitySource->CanIssueObjectHandles();
		probes.ObjectHandleValidationEnabled =
			g_EngineFacade && g_EngineFacade->IsConfigured();
		probes.FunctionHandleValidationEnabled =
			g_IdentitySource && g_IdentitySource->CanIssueFunctionHandles();
		probes.ObjectSnapshotPublished = g_EngineFacade
			&& g_EngineFacade->Snapshots().CurrentGeneration() != 0;
		probes.FunctionCallServiceEnabled = false;
		probes.LegacyHttpListening = legacyHttpListening;
		const auto capabilities = UExplorer::Runtime::BuildCoreCapabilities(*snapshot.Context, probes);
		if (!g_Runtime.PublishCapabilities(capabilities))
			return;

		std::vector<std::string> blockers;
		g_Runtime.TryMarkReady(UExplorer::Runtime::RequiredReadyCapabilities(), &blockers);
	}

	void StopFailedInitialization(HMODULE module, FILE* consoleFile, const char* code, const std::string& message)
	{
		UExplorer::Services::SetCoreCommandService(nullptr);
		g_CommandService.reset();
		g_EngineFacade.reset();
		g_IdentitySource.reset();
		g_Runtime.MarkFailed(code, message);
		g_Runtime.BeginStopping();
		g_Runtime.MarkStopped();
		UExplorer::Runtime::SetCoreRuntime(nullptr);
		if (consoleFile)
			fclose(consoleFile);
		FreeConsole();
		FreeLibraryAndExitThread(module, 1);
	}
}

static std::string TryProbeEngineVersionFromImage()
{
	HMODULE exeModule = GetModuleHandleW(nullptr);
	if (!exeModule)
		return "";

	const uint8* base = reinterpret_cast<const uint8*>(exeModule);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE)
		return "";

	if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x10000)
		return "";

	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
	if (!nt || nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage == 0)
		return "";

	const char* begin = reinterpret_cast<const char*>(base);
	const char* end = begin + nt->OptionalHeader.SizeOfImage;

	auto ScanWithTag = [begin, end](const char* tag) -> std::string
	{
		const size_t tagLen = std::strlen(tag);
		if (tagLen == 0 || static_cast<size_t>(end - begin) <= tagLen)
			return "";

		for (const char* p = begin; p + static_cast<ptrdiff_t>(tagLen + 4) < end; ++p)
		{
			if (std::memcmp(p, tag, tagLen) != 0)
				continue;

			const char* v = p + tagLen;
			std::string parsed;
			while (v < end)
			{
				const char ch = *v;
				if ((ch >= '0' && ch <= '9') || ch == '.')
				{
					parsed.push_back(ch);
					if (parsed.size() >= 16)
						break;
					++v;
					continue;
				}
				break;
			}

			if (parsed.find('.') != std::string::npos)
				return parsed;
		}

		return "";
	};

	std::string version = ScanWithTag("++UE4+Release-");
	if (version.empty())
		version = ScanWithTag("++UE5+Release-");
	if (version.empty())
		version = ScanWithTag("UE4+Release-");
	if (version.empty())
		version = ScanWithTag("UE5+Release-");

	return version;
}

static void PrimeGameVersionBeforeOffsetInit()
{
	if (!Settings::Generator::GameVersion.empty())
		return;

	const std::string probed = TryProbeEngineVersionFromImage();
	if (!probed.empty())
	{
		Settings::Generator::GameVersion = probed;
		std::cerr << "[UExplorer] Pre-init engine version probe: " << Settings::Generator::GameVersion << "\n";
	}
	else
	{
		std::cerr << "[UExplorer] Pre-init engine version probe: not found\n";
	}
}

static DWORD WINAPI MainThread(LPVOID lpParam)
{
	HMODULE Module = reinterpret_cast<HMODULE>(lpParam);

	AllocConsole();
	FILE* Dummy = nullptr;
	freopen_s(&Dummy, "CONOUT$", "w", stderr);
	freopen_s(&Dummy, "CONIN$", "r", stdin);

	std::cerr << "[UExplorer] Initializing...\n";
	UExplorer::Runtime::SetCoreRuntime(&g_Runtime);
	std::string coreSessionId;
	if (!UExplorer::Runtime::TryGenerateCoreSessionId(coreSessionId))
	{
		std::cerr << "[UExplorer] FATAL: secure Core session generation failed.\n";
		StopFailedInitialization(
			Module,
			Dummy,
			"CORE_SESSION_GENERATION_FAILED",
			"BCryptGenRandom could not create the Core session identity");
		return 1;
	}
	if (!g_Runtime.BeginInitialize(coreSessionId))
	{
		std::cerr << "[UExplorer] FATAL: CoreRuntime rejected initialization transition.\n";
		UExplorer::Runtime::SetCoreRuntime(nullptr);
		if (Dummy) fclose(Dummy);
		FreeConsole();
		FreeLibraryAndExitThread(Module, 1);
		return 1;
	}

	Settings::Config::Load(Module);

	if (Settings::Config::SleepTimeout > 0)
	{
		std::cerr << "[UExplorer] Sleeping for " << Settings::Config::SleepTimeout << "ms...\n";
		Sleep(Settings::Config::SleepTimeout);
	}

	PrimeGameVersionBeforeOffsetInit();

	try
	{
		Generator::InitEngineCore();
		Generator::InitInternal();
		const auto context = UExplorer::Runtime::CaptureEngineContext(1);
		if (!g_Runtime.PublishContext(context))
			throw std::runtime_error("CoreRuntime rejected immutable EngineContext publication");
	}
	catch (const std::exception& e)
	{
		std::cerr << "[UExplorer] FATAL: Engine init failed: " << e.what() << "\n";
		StopFailedInitialization(Module, Dummy, "ENGINE_INITIALIZATION_FAILED", e.what());
		return 1;
	}
	catch (...)
	{
		std::cerr << "[UExplorer] FATAL: Engine init crashed with unknown exception.\n";
		StopFailedInitialization(
			Module,
			Dummy,
			"ENGINE_INITIALIZATION_EXCEPTION",
			"Engine initialization raised an unknown exception");
		return 1;
	}

	std::cerr << "[UExplorer] Engine core initialized.\n";
	try
	{
		const UExplorer::Runtime::CoreRuntimeSnapshot runtimeSnapshot = g_Runtime.Snapshot();
		if (!runtimeSnapshot.Context)
			throw std::runtime_error("Immutable EngineContext is unavailable");
		g_IdentitySource = std::make_unique<UExplorer::Runtime::ObjectArrayIdentitySource>(
			runtimeSnapshot.Context);
		g_EngineFacade = std::make_unique<UExplorer::Runtime::EngineFacade>(
			runtimeSnapshot.Context,
			runtimeSnapshot.SessionId,
			*g_IdentitySource);
		if (!g_EngineFacade->IsConfigured())
			throw std::runtime_error("EngineFacade rejected the runtime session/context");
		g_CommandService = std::make_unique<UExplorer::Services::CoreCommandService>(
			g_Runtime,
			UExplorer::Runtime::GetGameThreadExecutor(),
			*g_EngineFacade,
			g_StatusDiagnostics);
		if (!g_CommandService->IsConfigured())
			throw std::runtime_error("Core command service rejected the runtime session/context");
		UExplorer::Services::SetCoreCommandService(g_CommandService.get());
	}
	catch (const std::exception& e)
	{
		std::cerr << "[UExplorer] FATAL: command service init failed: " << e.what() << "\n";
		StopFailedInitialization(Module, Dummy, "COMMAND_SERVICE_INITIALIZATION_FAILED", e.what());
		return 1;
	}

	// Startup runs on an owned worker thread. Do not call ProcessEvent here;
	// unresolved metadata remains unavailable until a verified game-thread command exists.

	std::cerr << "[UExplorer] Game: " << Settings::Generator::GameName << "\n";
	std::cerr << "[UExplorer] Version: " << Settings::Generator::GameVersion << "\n";

	uint16_t configuredPort = kDefaultApiPort;
	std::string configuredToken = kDefaultApiToken;
	LoadConnectionConfig(configuredPort, configuredToken);
	std::cerr << "[UExplorer] Connection config: preferred_port="
		<< configuredPort << " token_len=" << configuredToken.size()
		<< " auth_mode=" << kAuthMode << "\n";

	// Start HTTP server
	const uint16_t port = configuredPort;
	const std::string token = configuredToken;

	bool startupReady = false;
	bool unloadSafe = true;
	try
	{
		g_Server = std::make_unique<UExplorer::HttpServer>(port, token);
		UExplorer::API::RegisterAllRoutes(*g_Server);
		if (!g_Server->Start())
		{
			std::cerr << "[UExplorer] Failed to bind configured HTTP port " << port << "\n";
		}
		else
		{
			UExplorer::API::SetServer(g_Server.get());
			if (!UExplorer::API::InitHooks())
			{
				std::cerr << "[UExplorer] Failed to initialize required game-thread hook\n";
			}
			else
			{
				const uint16_t actualPort = g_Server->GetPort();
				WriteRuntimeState(actualPort, token, true);
				RefreshRuntimeCapabilities(true);
				startupReady = true;
			}
		}
	}
	catch (const std::exception& e)
	{
		std::cerr << "[UExplorer] Failed to start HTTP server: " << e.what() << "\n";
	}

	// Keep alive only after every required startup stage succeeds.
	while (startupReady && g_Running.load())
	{
		RefreshRuntimeCapabilities(true);
		if (GetAsyncKeyState(VK_F6) & 1)
		{
			g_Running.store(false);
			break;
		}
		Sleep(100);
	}

	std::cerr << "[UExplorer] Shutting down...\n";

	g_Runtime.BeginStopping();
	UExplorer::Services::SetCoreCommandService(nullptr);
	UExplorer::API::SetServer(nullptr);
	bool serverStopped = true;
	bool dumpStopped = true;
	bool hooksStopped = true;
	UExplorer::Runtime::ShutdownCoordinator shutdown;
	shutdown.AddStage("legacy_http", [&] {
		serverStopped = !g_Server || g_Server->Stop();
		return serverStopped;
	});
	shutdown.AddStage("dump_jobs", [&] {
		dumpStopped = UExplorer::API::ShutdownDumpJobs();
		return dumpStopped;
	});
	shutdown.AddStage("hooks", [&] {
		hooksStopped = UExplorer::API::ShutdownHooks();
		return hooksStopped;
	});
	shutdown.AddStage("engine_facade", [&] {
		return !g_EngineFacade
			|| g_EngineFacade->Stop(std::chrono::milliseconds(5000));
	});
	shutdown.AddStage("runtime_requests", [&] {
		return g_Runtime.WaitForRequests(std::chrono::milliseconds(5000));
	});
	const UExplorer::Runtime::ShutdownReport shutdownReport = shutdown.Run();
	unloadSafe = unloadSafe && shutdownReport.SafeToUnload;
	if (serverStopped)
		g_Server.reset();
	WriteRuntimeState(0, token, false);

	if (!unloadSafe)
	{
		g_Runtime.RecordShutdownFailure(
			"SHUTDOWN_SAFETY_NOT_PROVEN",
			"At least one owned runtime stage did not stop safely");
		std::cerr << "[UExplorer] Shutdown could not prove all workers/hooks drained; DLL remains loaded.\n";
		if (!dumpStopped)
		{
			std::cerr << "[UExplorer] Waiting for the non-cancellable dump worker before releasing its thread owner.\n";
			while (!UExplorer::API::ShutdownDumpJobs(1000))
				Sleep(100);
		}
		if (Dummy) fclose(Dummy);
		FreeConsole();
		return 1;
	}
	g_CommandService.reset();
	g_EngineFacade.reset();
	g_IdentitySource.reset();
	if (!g_Runtime.MarkStopped())
	{
		g_Runtime.RecordShutdownFailure(
			"RUNTIME_STOP_TRANSITION_FAILED",
			"CoreRuntime could not enter Stopped after owned stages drained");
		std::cerr << "[UExplorer] CoreRuntime stop transition failed; DLL remains loaded.\n";
		if (Dummy) fclose(Dummy);
		FreeConsole();
		return 1;
	}

	UExplorer::Runtime::SetCoreRuntime(nullptr);
	if (Dummy) fclose(Dummy);
	FreeConsole();
	FreeLibraryAndExitThread(Module, 0);

	return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		g_Module = hModule;
		DisableThreadLibraryCalls(hModule);
		if (HANDLE thread = CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr))
			CloseHandle(thread);
		else
			return FALSE;
		break;
	}
	return TRUE;
}
