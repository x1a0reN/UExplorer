#include <Windows.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <vector>

#include "Generators/Generator.h"
#include "IPC/NamedPipeRpcServer.h"
#include "Runtime/CoreCapabilities.h"
#include "Runtime/CoreRuntimeAccess.h"
#include "Runtime/CoreSession.h"
#include "Runtime/EngineContextCapture.h"
#include "Runtime/EngineFacade.h"
#include "Runtime/EngineVersionProbe.h"
#include "Runtime/GameThreadExecutor.h"
#include "Runtime/ObjectArrayIdentitySource.h"
#include "Runtime/ObjectArraySnapshotSource.h"
#include "Runtime/PostRenderHook.h"
#include "Runtime/ShutdownCoordinator.h"
#include "Services/CoreCommandService.h"
#include "Services/CoreCommandServiceAccess.h"
#include "Services/CoreStatusDiagnostics.h"
#include "Settings.h"
#include "OffsetFinder/Offsets.h"

static std::atomic<bool> g_Running{ true };
static UExplorer::Runtime::CoreRuntime g_Runtime;
static UExplorer::Services::EngineCoreStatusDiagnosticsSource g_StatusDiagnostics;
static std::unique_ptr<UExplorer::IPC::NamedPipeRpcServer> g_PipeServer;
static std::unique_ptr<UExplorer::Runtime::ObjectArrayIdentitySource> g_IdentitySource;
static std::unique_ptr<UExplorer::Runtime::EngineFacade> g_EngineFacade;
static std::unique_ptr<UExplorer::Runtime::ObjectArraySnapshotSource> g_SnapshotSource;
static std::unique_ptr<UExplorer::Services::CoreCommandService> g_CommandService;
static std::unique_ptr<UExplorer::Runtime::PostRenderHook> g_PostRenderHook;
static bool g_SnapshotPumpAttached = false;

namespace
{
	void RefreshRuntimeCapabilities()
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
		probes.ReflectionLayoutValidated = false;
		const auto propertyCodec = g_EngineFacade
			? g_EngineFacade->Properties()
			: nullptr;
		probes.PropertyCodecEnabled = propertyCodec && propertyCodec->IsConfigured();
		probes.FunctionCallServiceEnabled = false;
		probes.NamedPipeListening = g_PipeServer && g_PipeServer->IsListening();
		const auto capabilities = UExplorer::Runtime::BuildCoreCapabilities(*snapshot.Context, probes);
		if (!g_Runtime.PublishCapabilities(capabilities))
			return;

		std::vector<std::string> blockers;
		g_Runtime.TryMarkReady(UExplorer::Runtime::RequiredReadyCapabilities(), &blockers);
	}

	bool EnsurePipeAdmissions()
	{
		if (!g_PipeServer || !g_PipeServer->IsListening())
			return false;
		if (g_PipeServer->Diagnostics().AdmissionsOpen)
			return true;
		if (!g_Runtime.Snapshot().IsReady())
			return true;
		return g_PipeServer->OpenAdmissions();
	}

	void StopFailedInitialization(HMODULE module, FILE* consoleFile, const char* code, const std::string& message)
	{
		if (g_PostRenderHook
			&& !g_PostRenderHook->Stop(std::chrono::milliseconds(5000)))
		{
			g_Runtime.RecordShutdownFailure(
				"POST_RENDER_INITIALIZATION_STOP_TIMEOUT",
				"PostRender hook did not restore after initialization failed");
			std::cerr << "[UExplorer] PostRender hook did not restore; DLL remains loaded.\n";
			if (consoleFile)
				fclose(consoleFile);
			FreeConsole();
			ExitThread(1);
		}
		g_PostRenderHook.reset();
		if (g_PipeServer
			&& !g_PipeServer->Stop(std::chrono::milliseconds(5000)))
		{
			g_Runtime.RecordShutdownFailure(
				"PIPE_INITIALIZATION_STOP_TIMEOUT",
				"Named-pipe threads did not drain after initialization failed");
			std::cerr << "[UExplorer] Named-pipe threads did not drain; DLL remains loaded.\n";
			if (consoleFile)
				fclose(consoleFile);
			FreeConsole();
			ExitThread(1);
		}
		g_PipeServer.reset();
		UExplorer::Services::SetCoreCommandService(nullptr);
		g_CommandService.reset();
		g_EngineFacade.reset();
		g_SnapshotSource.reset();
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

static void PrimeGameVersionBeforeOffsetInit()
{
	if (!Settings::Generator::GameVersion.empty())
		return;

	const UExplorer::Runtime::EngineVersionProbeResult probe =
		UExplorer::Runtime::ProbeLoadedEngineVersion();
	if (probe.Ok())
	{
		Settings::Generator::GameVersion = probe.Version;
		std::cerr << "[UExplorer] Pre-init engine version probe: " << Settings::Generator::GameVersion << "\n";
	}
	else
	{
		std::cerr << "[UExplorer] Pre-init engine version probe failed: code="
			<< UExplorer::Runtime::ToString(probe.Error)
			<< " image=" << UExplorer::Platform::ToString(probe.ImageError)
			<< " memory=" << UExplorer::Runtime::ToString(probe.MemoryFailure)
			<< " native=" << probe.NativeError << "\n";
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
		if (g_EngineFacade->Names().IsConfigured() && g_IdentitySource->CanReadObjectSlots())
		{
			g_SnapshotSource = std::make_unique<UExplorer::Runtime::ObjectArraySnapshotSource>(
				runtimeSnapshot.Context,
				*g_EngineFacade,
				*g_IdentitySource);
			if (!g_SnapshotSource->IsConfigured()
				|| !g_EngineFacade->ConfigureSnapshotCapture(*g_SnapshotSource))
			{
				throw std::runtime_error("Production object snapshot source rejected the immutable context");
			}
		}
		else
		{
			std::cerr << "[UExplorer] Object snapshot unavailable: immutable names or serial-backed object slots are not validated.\n";
		}
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

	try
	{
		g_PipeServer = std::make_unique<UExplorer::IPC::NamedPipeRpcServer>(
			g_Runtime,
			*g_CommandService,
			UExplorer::Runtime::GetGameThreadExecutor(),
			[] { g_Running.store(false, std::memory_order_release); });
		if (!g_PipeServer->Start())
		{
			const UExplorer::IPC::NamedPipeServerDiagnostics diagnostics =
				g_PipeServer->Diagnostics();
			throw std::runtime_error(
				diagnostics.LastErrorCode + ": " + diagnostics.LastErrorMessage
				+ " (native=" + std::to_string(diagnostics.LastNativeError) + ")");
		}
		std::wcerr << L"[UExplorer] Named Pipe bound: " << g_PipeServer->PipeName() << L"\n";
	}
	catch (const std::exception& e)
	{
		std::cerr << "[UExplorer] FATAL: Named Pipe startup failed: " << e.what() << "\n";
		StopFailedInitialization(Module, Dummy, "PIPE_INITIALIZATION_FAILED", e.what());
		return 1;
	}

	// Startup runs on an owned worker thread. Do not call ProcessEvent here;
	// unresolved metadata remains unavailable until a verified game-thread command exists.

	std::cerr << "[UExplorer] Game: " << Settings::Generator::GameName << "\n";
	std::cerr << "[UExplorer] Version: " << Settings::Generator::GameVersion << "\n";

	bool startupReady = false;
	bool unloadSafe = true;
	try
	{
		g_PostRenderHook = std::make_unique<UExplorer::Runtime::PostRenderHook>();
		if (!g_PostRenderHook->Install())
		{
			throw std::runtime_error("required PostRender game-thread pump could not be installed");
		}
		if (UExplorer::Runtime::EngineSnapshotCapture* capture =
			g_EngineFacade ? g_EngineFacade->SnapshotCapture() : nullptr)
		{
			auto& pump = UExplorer::Runtime::GetPostRenderPumpBackend();
			if (!pump.AttachFrameClient(*capture))
			{
				std::cerr << "[UExplorer] Object snapshot unavailable: PostRender frame client attachment failed.\n";
			}
			else
			{
				g_SnapshotPumpAttached = true;
				const UExplorer::Runtime::SnapshotCaptureRequestResult requested =
					capture->RequestCapture();
				if (!requested.Ok())
				{
					std::cerr << "[UExplorer] Initial object snapshot request failed: "
						<< UExplorer::Runtime::ToString(requested.Error) << "\n";
				}
			}
		}
		RefreshRuntimeCapabilities();
		startupReady = EnsurePipeAdmissions();
		if (!startupReady)
			throw std::runtime_error("Named Pipe lost readiness before admissions opened");
	}
	catch (const std::exception& e)
	{
		std::cerr << "[UExplorer] FATAL: runtime startup failed: " << e.what() << "\n";
		StopFailedInitialization(
			Module,
			Dummy,
			"RUNTIME_STARTUP_FAILED",
			e.what());
		return 1;
	}

	// Keep alive only after every required startup stage succeeds.
	auto nextSnapshotRefresh = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (startupReady && g_Running.load())
	{
		RefreshRuntimeCapabilities();
		if (!EnsurePipeAdmissions())
		{
			std::cerr << "[UExplorer] Named Pipe listener/admission contract failed.\n";
			g_Running.store(false, std::memory_order_release);
			break;
		}
		const auto now = std::chrono::steady_clock::now();
		if (g_SnapshotPumpAttached && now >= nextSnapshotRefresh)
		{
			if (UExplorer::Runtime::EngineSnapshotCapture* capture =
				g_EngineFacade ? g_EngineFacade->SnapshotCapture() : nullptr)
			{
				const UExplorer::Runtime::SnapshotCaptureState state = capture->Diagnostics().State;
				if (state == UExplorer::Runtime::SnapshotCaptureState::Idle
					|| state == UExplorer::Runtime::SnapshotCaptureState::Completed
					|| state == UExplorer::Runtime::SnapshotCaptureState::Failed)
				{
					const UExplorer::Runtime::SnapshotCaptureRequestResult requested =
						capture->RequestCapture();
					if (!requested.Ok())
					{
						std::cerr << "[UExplorer] Object snapshot refresh request failed: "
							<< UExplorer::Runtime::ToString(requested.Error) << "\n";
					}
				}
			}
			nextSnapshotRefresh = now + std::chrono::seconds(2);
		}
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
	bool pipeStopped = true;
	bool hooksStopped = true;
	bool snapshotPumpStopped = true;
	UExplorer::Runtime::ShutdownCoordinator shutdown;
	shutdown.AddStage("named_pipe", [&] {
		pipeStopped = !g_PipeServer
			|| g_PipeServer->Stop(std::chrono::milliseconds(5000));
		return pipeStopped;
	});
	shutdown.AddStage("post_render_hook", [&] {
		hooksStopped = !g_PostRenderHook
			|| g_PostRenderHook->Stop(std::chrono::milliseconds(5000));
		return hooksStopped;
	});
	shutdown.AddStage("snapshot_pump", [&] {
		if (!g_SnapshotPumpAttached)
			return true;
		UExplorer::Runtime::EngineSnapshotCapture* capture =
			g_EngineFacade ? g_EngineFacade->SnapshotCapture() : nullptr;
		if (!capture)
			return false;
		snapshotPumpStopped = UExplorer::Runtime::GetPostRenderPumpBackend().DetachFrameClient(
			*capture,
			std::chrono::milliseconds(5000));
		if (snapshotPumpStopped)
			g_SnapshotPumpAttached = false;
		return snapshotPumpStopped;
	});
	shutdown.AddStage("engine_facade", [&] {
		return snapshotPumpStopped
			&& (!g_EngineFacade
				|| g_EngineFacade->Stop(std::chrono::milliseconds(5000)));
	});
	shutdown.AddStage("runtime_requests", [&] {
		return g_Runtime.WaitForRequests(std::chrono::milliseconds(5000));
	});
	const UExplorer::Runtime::ShutdownReport shutdownReport = shutdown.Run();
	unloadSafe = unloadSafe && shutdownReport.SafeToUnload;
	if (pipeStopped)
		g_PipeServer.reset();
	if (hooksStopped)
		g_PostRenderHook.reset();

	if (!unloadSafe)
	{
		g_Runtime.RecordShutdownFailure(
			"SHUTDOWN_SAFETY_NOT_PROVEN",
			"At least one owned runtime stage did not stop safely");
		std::cerr << "[UExplorer] Shutdown could not prove all workers/hooks drained; DLL remains loaded.\n";
		if (Dummy) fclose(Dummy);
		FreeConsole();
		return 1;
	}
	g_CommandService.reset();
	g_EngineFacade.reset();
	g_SnapshotSource.reset();
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
		DisableThreadLibraryCalls(hModule);
		if (HANDLE thread = CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr))
			CloseHandle(thread);
		else
			return FALSE;
		break;
	}
	return TRUE;
}
