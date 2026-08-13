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
#include "Runtime/BlueprintBytecodeRuntime.h"
#include "Runtime/CoreCapabilities.h"
#include "Runtime/CoreRuntimeAccess.h"
#include "Runtime/CoreSession.h"
#include "Runtime/DumpJobCoordinator.h"
#include "Runtime/EngineContextCapture.h"
#include "Runtime/EngineFacade.h"
#include "Runtime/EngineVersionProbe.h"
#include "Runtime/FunctionCallBatchCoordinator.h"
#include "Runtime/GameThreadExecutor.h"
#include "Runtime/GameThreadFrameScheduler.h"
#include "Runtime/HookEventCollector.h"
#include "Runtime/ObjectArrayIdentitySource.h"
#include "Runtime/ObjectArraySnapshotSource.h"
#include "Runtime/ObjectSnapshotReflectionCandidateSource.h"
#include "Runtime/ObjectSnapshotTypeCandidateSource.h"
#include "Runtime/PostRenderHook.h"
#include "Runtime/ShutdownCoordinator.h"
#include "Runtime/WatchScheduler.h"
#include "Services/CoreCommandService.h"
#include "Services/CoreCommandServiceAccess.h"
#include "Services/CoreStatusDiagnostics.h"
#include "Services/DumpCommandService.h"
#include "Services/DomainEventPump.h"
#include "Services/FunctionCallBatchCommandService.h"
#include "Services/HookCommandService.h"
#include "Services/ProcessEventHookOwner.h"
#include "Services/SnapshotDumpWorker.h"
#include "Services/WatchCommandService.h"
#include "Settings.h"
#include "OffsetFinder/Offsets.h"

static std::atomic<bool> g_Running{ true };
static UExplorer::Runtime::CoreRuntime g_Runtime;
static UExplorer::Services::EngineCoreStatusDiagnosticsSource g_StatusDiagnostics;
static std::unique_ptr<UExplorer::IPC::NamedPipeRpcServer> g_PipeServer;
static std::unique_ptr<UExplorer::Runtime::ObjectArrayIdentitySource> g_IdentitySource;
static std::unique_ptr<UExplorer::Runtime::EngineFacade> g_EngineFacade;
static std::unique_ptr<UExplorer::Runtime::ObjectArraySnapshotSource> g_SnapshotSource;
static std::unique_ptr<UExplorer::Runtime::ObjectSnapshotReflectionCandidateSource>
	g_ReflectionSource;
static std::unique_ptr<UExplorer::Runtime::ObjectSnapshotTypeCandidateSource>
	g_TypeSource;
static std::unique_ptr<UExplorer::Services::CoreCommandService> g_CommandService;
static std::shared_ptr<UExplorer::Services::FunctionCallBatchExactInvokeAdapter>
	g_CallBatchAdapter;
static std::shared_ptr<UExplorer::Services::FunctionCallBatchCommandWorker>
	g_CallBatchWorker;
static std::unique_ptr<UExplorer::Runtime::FunctionCallBatchCoordinator>
	g_CallBatchCoordinator;
static std::unique_ptr<UExplorer::Services::FunctionCallBatchCommandService>
	g_CallBatchCommandService;
static std::unique_ptr<UExplorer::Runtime::BlueprintBytecodeRuntimeSource>
	g_BlueprintBytecodeSource;
static std::unique_ptr<UExplorer::Services::ObjectPropertyWatchSampleSource> g_WatchSource;
static std::unique_ptr<UExplorer::Runtime::WatchScheduler> g_WatchScheduler;
static std::unique_ptr<UExplorer::Runtime::HookEventCollector> g_HookCollector;
static std::unique_ptr<UExplorer::Services::HookCommandService> g_HookCommandService;
static std::unique_ptr<UExplorer::Services::DomainEventPump> g_DomainEventPump;
static std::unique_ptr<UExplorer::Services::ProcessEventHookOwner> g_ProcessEventHook;
static std::shared_ptr<UExplorer::Services::SnapshotDumpWorker> g_DumpWorker;
static std::unique_ptr<UExplorer::Runtime::DumpJobCoordinator> g_DumpCoordinator;
static std::unique_ptr<UExplorer::Services::DumpCommandService> g_DumpCommandService;
static std::unique_ptr<UExplorer::Runtime::PostRenderHook> g_PostRenderHook;
static bool g_FrameSchedulerPumpAttached = false;
static bool g_SnapshotFrameClientAttached = false;
static bool g_ReflectionFrameClientAttached = false;
static bool g_TypeFrameClientAttached = false;
static bool g_WorldFrameClientAttached = false;
static bool g_WatchFrameClientAttached = false;

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
		const std::shared_ptr<const UExplorer::Runtime::EngineSnapshot> objectSnapshot =
			g_EngineFacade ? g_EngineFacade->Snapshots().Current() : nullptr;
		probes.ObjectSnapshotPublished = static_cast<bool>(objectSnapshot);
		probes.ObjectSnapshotGeneration = objectSnapshot ? objectSnapshot->Generation : 0;
		probes.Reflection = g_EngineFacade
			? g_EngineFacade->Reflection()
			: nullptr;
		probes.Types = g_EngineFacade
			? g_EngineFacade->Types().Current()
			: nullptr;
		probes.ObjectPropertyServiceEnabled = true;
		probes.FunctionCallServiceEnabled = true;
		probes.FunctionCallBatchCommandServiceEnabled = g_CallBatchCommandService
			&& g_CallBatchCommandService->IsConfigured();
		probes.BlueprintBytecodeCaptureEnabled = g_BlueprintBytecodeSource
			&& g_BlueprintBytecodeSource->IsCaptureConfigured();
		// Raw Script capture is witnessed independently. No exact opcode/operand
		// profile is published until its compile-time UE layout is also proved.
		probes.BlueprintBytecodeProfileEnabled = false;
		probes.MemoryReadCommandServiceEnabled = true;
		probes.MemoryWriteCommandServiceEnabled = true;
		probes.WatchCommandServiceEnabled = g_WatchScheduler
			&& g_WatchScheduler->IsConfigured()
			&& g_WatchFrameClientAttached;
		const UExplorer::Services::DomainEventPumpDiagnostics domainEvents =
			g_DomainEventPump
				? g_DomainEventPump->Diagnostics()
				: UExplorer::Services::DomainEventPumpDiagnostics{};
		probes.DomainEventPumpEnabled = domainEvents.Configured
			&& domainEvents.Started
			&& !domainEvents.StopRequested
			&& !domainEvents.Stopped;
		probes.HookCommandServiceEnabled = g_HookCommandService
			&& g_HookCommandService->IsConfigured();
		const UExplorer::Services::ProcessEventHookDiagnostics hook =
			g_ProcessEventHook
				? g_ProcessEventHook->Diagnostics()
				: UExplorer::Services::ProcessEventHookDiagnostics{};
		probes.HookProducerInstalled = hook.Configured
			&& hook.Installed
			&& hook.CoverageComplete
			&& probes.Types
			&& objectSnapshot
			&& hook.CoveredObjectSnapshotGeneration == objectSnapshot->Generation
			&& hook.CoveredTypeSnapshotGeneration == probes.Types->Generation();
		probes.DumpCommandServiceEnabled = static_cast<bool>(g_DumpCommandService);
		probes.DumpWorkerEnabled = g_DumpWorker
			&& g_DumpWorker->IsConfigured()
			&& g_DumpCoordinator
			&& g_DumpCoordinator->IsConfigured();
		probes.World = g_EngineFacade
			? g_EngineFacade->Worlds().Current()
			: nullptr;
		probes.WorldInspectServiceEnabled =
			g_EngineFacade && g_EngineFacade->WorldCapture();
		probes.WorldMutationServiceEnabled = true;
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

	bool IsReflectionCaptureActive(
		const UExplorer::Runtime::ReflectionLayoutCaptureState state) noexcept
	{
		using UExplorer::Runtime::ReflectionLayoutCaptureState;
		return state == ReflectionLayoutCaptureState::Requested
			|| state == ReflectionLayoutCaptureState::Capturing
			|| state == ReflectionLayoutCaptureState::Validating
			|| state == ReflectionLayoutCaptureState::Publishing;
	}

	bool IsSnapshotCaptureActive(
		const UExplorer::Runtime::SnapshotCaptureState state) noexcept
	{
		using UExplorer::Runtime::SnapshotCaptureState;
		return state == SnapshotCaptureState::Requested
			|| state == SnapshotCaptureState::Capturing
			|| state == SnapshotCaptureState::Validating
			|| state == SnapshotCaptureState::Publishing;
	}

	bool IsTypeCapturePumping(
		const UExplorer::Runtime::TypeSnapshotCaptureState state) noexcept
	{
		using UExplorer::Runtime::TypeSnapshotCaptureState;
		return state == TypeSnapshotCaptureState::Requested
			|| state == TypeSnapshotCaptureState::Capturing
			|| state == TypeSnapshotCaptureState::Validating
			|| state == TypeSnapshotCaptureState::Sealing
			|| state == TypeSnapshotCaptureState::Publishing;
	}

	bool IsWorldCaptureActive(
		const UExplorer::Runtime::WorldSnapshotCaptureState state) noexcept
	{
		using UExplorer::Runtime::WorldSnapshotCaptureState;
		return state == WorldSnapshotCaptureState::Requested
			|| state == WorldSnapshotCaptureState::Scanning
			|| state == WorldSnapshotCaptureState::Sealing
			|| state == WorldSnapshotCaptureState::Validating
			|| state == WorldSnapshotCaptureState::Publishing;
	}

	bool DetachWorldFrameClient(const std::chrono::milliseconds timeout)
	{
		if (!g_WorldFrameClientAttached)
			return true;
		UExplorer::Runtime::WorldSnapshotCapture* capture =
			g_EngineFacade ? g_EngineFacade->WorldCapture() : nullptr;
		if (!capture
			|| !UExplorer::Runtime::GetGameThreadFrameScheduler().DetachClient(
				*capture,
				timeout))
		{
			return false;
		}
		g_WorldFrameClientAttached = false;
		return true;
	}

	bool DetachWatchFrameClient(const std::chrono::milliseconds timeout)
	{
		if (!g_WatchFrameClientAttached)
			return true;
		if (!g_WatchScheduler
			|| !UExplorer::Runtime::GetGameThreadFrameScheduler().DetachClient(
				*g_WatchScheduler,
				timeout))
		{
			return false;
		}
		g_WatchFrameClientAttached = false;
		return true;
	}

	bool DetachTypeFrameClient(const std::chrono::milliseconds timeout)
	{
		if (!g_TypeFrameClientAttached)
			return true;
		UExplorer::Runtime::TypeSnapshotCapture* capture =
			g_EngineFacade ? g_EngineFacade->TypeCapture() : nullptr;
		if (!capture
			|| !UExplorer::Runtime::GetGameThreadFrameScheduler().DetachClient(
				*capture,
				timeout))
		{
			return false;
		}
		g_TypeFrameClientAttached = false;
		return true;
	}

	bool DetachReflectionFrameClient(const std::chrono::milliseconds timeout)
	{
		if (!g_ReflectionFrameClientAttached)
			return true;
		UExplorer::Runtime::ReflectionLayoutCapture* capture =
			g_EngineFacade ? g_EngineFacade->ReflectionCapture() : nullptr;
		if (!capture
			|| !UExplorer::Runtime::GetGameThreadFrameScheduler().DetachClient(
				*capture,
				timeout))
		{
			return false;
		}
		g_ReflectionFrameClientAttached = false;
		return true;
	}

	bool ReflectionCaptureBlocksSnapshotRefresh() noexcept
	{
		const UExplorer::Runtime::ReflectionLayoutCapture* capture =
			g_EngineFacade ? g_EngineFacade->ReflectionCapture() : nullptr;
		return capture
			&& IsReflectionCaptureActive(capture->Diagnostics().State);
	}

	bool TypeCaptureBlocksSnapshotRefresh() noexcept
	{
		const UExplorer::Runtime::TypeSnapshotCapture* capture =
			g_EngineFacade ? g_EngineFacade->TypeCapture() : nullptr;
		if (!capture)
			return false;
		const UExplorer::Runtime::TypeSnapshotCaptureState state =
			capture->Diagnostics().State;
		return IsTypeCapturePumping(state)
			|| state == UExplorer::Runtime::TypeSnapshotCaptureState::Ready;
	}

	bool WorldCaptureBlocksSnapshotRefresh() noexcept
	{
		const UExplorer::Runtime::WorldSnapshotCapture* capture =
			g_EngineFacade ? g_EngineFacade->WorldCapture() : nullptr;
		return capture && IsWorldCaptureActive(capture->Diagnostics().State);
	}

	bool DriveReflectionDiscovery(
		std::uint64_t& lastPreparationGeneration,
		std::uint64_t& lastReportedFailureGeneration)
	{
		if (!g_EngineFacade || !g_ReflectionSource)
			return true;

		UExplorer::Runtime::ReflectionLayoutCapture* capture =
			g_EngineFacade->ReflectionCapture();
		if (g_EngineFacade->Reflection())
		{
			if (!DetachReflectionFrameClient(std::chrono::milliseconds(5000)))
				return false;
			return g_ReflectionSource->ReleasePreparedPlan();
		}

		if (capture)
		{
			const UExplorer::Runtime::ReflectionLayoutCaptureDiagnostics diagnostics =
				capture->Diagnostics();
			if (IsReflectionCaptureActive(diagnostics.State))
				return true;
			if (diagnostics.State == UExplorer::Runtime::ReflectionLayoutCaptureState::Failed)
			{
				const auto sourceDiagnostics = g_ReflectionSource->Diagnostics();
				const std::uint64_t failedGeneration =
					sourceDiagnostics.PreparedSnapshotGeneration != 0
						? sourceDiagnostics.PreparedSnapshotGeneration
						: lastPreparationGeneration;
				if (lastReportedFailureGeneration != failedGeneration)
				{
					std::cerr << "[UExplorer] Reflection capture failed: capture="
						<< UExplorer::Runtime::ToString(diagnostics.Error)
						<< " source=" << UExplorer::Runtime::ToString(diagnostics.SourceError)
						<< " validation="
						<< UExplorer::Runtime::ToString(diagnostics.ValidationError)
						<< " snapshot_generation=" << failedGeneration << "\n";
					lastReportedFailureGeneration = failedGeneration;
				}
				if (!DetachReflectionFrameClient(std::chrono::milliseconds(5000))
					|| !g_ReflectionSource->ReleasePreparedPlan())
				{
					return false;
				}
			}
			else if (diagnostics.State
				== UExplorer::Runtime::ReflectionLayoutCaptureState::Completed)
			{
				if (!DetachReflectionFrameClient(std::chrono::milliseconds(5000))
					|| !g_ReflectionSource->ReleasePreparedPlan())
				{
					return false;
				}
				return true;
			}
		}

		const UExplorer::Runtime::EngineSnapshotCapture* snapshotCapture =
			g_EngineFacade->SnapshotCapture();
		if (snapshotCapture
			&& IsSnapshotCaptureActive(snapshotCapture->Diagnostics().State))
		{
			return true;
		}
		const std::shared_ptr<const UExplorer::Runtime::EngineSnapshot> snapshot =
			g_EngineFacade->Snapshots().Current();
		if (!snapshot || snapshot->Generation == lastPreparationGeneration)
			return true;

		lastPreparationGeneration = snapshot->Generation;
		const UExplorer::Runtime::ReflectionCandidatePreparationResult prepared =
			g_ReflectionSource->Prepare();
		if (!prepared.Ok())
		{
			std::cerr << "[UExplorer] Reflection preparation unavailable: code="
				<< UExplorer::Runtime::ToString(prepared.Error)
				<< " snapshot_generation=" << snapshot->Generation;
			if (!prepared.EvidencePath.empty())
				std::cerr << " evidence=" << prepared.EvidencePath;
			std::cerr << "\n";
			return prepared.Error == UExplorer::Runtime::ReflectionCandidatePreparationError::Busy
				|| g_ReflectionSource->ReleasePreparedPlan();
		}

		capture = g_EngineFacade->ReflectionCapture();
		if (!capture)
		{
			if (!g_EngineFacade->ConfigureReflectionCapture(
				*g_ReflectionSource,
				true))
			{
				std::cerr << "[UExplorer] Reflection capture ownership configuration failed.\n";
				return g_ReflectionSource->ReleasePreparedPlan();
			}
			capture = g_EngineFacade->ReflectionCapture();
		}
		if (!capture)
			return false;

		if (!g_ReflectionFrameClientAttached)
		{
			if (!UExplorer::Runtime::GetGameThreadFrameScheduler().AttachClient(*capture))
			{
				std::cerr << "[UExplorer] Reflection frame-client attachment failed.\n";
				return g_ReflectionSource->ReleasePreparedPlan();
			}
			g_ReflectionFrameClientAttached = true;
		}
		const UExplorer::Runtime::ReflectionLayoutCaptureError requested =
			capture->RequestCapture();
		if (requested != UExplorer::Runtime::ReflectionLayoutCaptureError::None)
		{
			std::cerr << "[UExplorer] Reflection capture request failed: "
				<< UExplorer::Runtime::ToString(requested) << "\n";
			if (IsReflectionCaptureActive(capture->Diagnostics().State))
				return true;
			return DetachReflectionFrameClient(std::chrono::milliseconds(5000))
				&& g_ReflectionSource->ReleasePreparedPlan();
		}
		return true;
	}

	bool DriveTypeDiscovery(
		std::uint64_t& lastPreparationGeneration,
		std::uint64_t& lastPreparationFingerprint,
		std::uint64_t& lastReportedFailureGeneration,
		std::uint64_t& lastReportedFailureFingerprint)
	{
		if (!g_EngineFacade || !g_TypeSource)
			return true;

		UExplorer::Runtime::TypeSnapshotCapture* capture =
			g_EngineFacade->TypeCapture();
		if (capture)
		{
			const UExplorer::Runtime::TypeSnapshotCaptureDiagnostics diagnostics =
				capture->Diagnostics();
			if (IsTypeCapturePumping(diagnostics.State))
				return true;
			if (diagnostics.State == UExplorer::Runtime::TypeSnapshotCaptureState::Ready)
			{
				if (!DetachTypeFrameClient(std::chrono::milliseconds(5000)))
					return false;
				const UExplorer::Runtime::TypeSnapshotPublishResult published =
					capture->PublishReady();
				(void)capture->ReclaimRetired();
				if (!g_TypeSource->ReleasePreparedPlan())
					return false;
				if (!published.Ok())
				{
					std::cerr << "[UExplorer] Type snapshot publication failed: "
						<< UExplorer::Runtime::ToString(published.Error) << "\n";
				}
			}
			else if (diagnostics.State == UExplorer::Runtime::TypeSnapshotCaptureState::Failed)
			{
				if (lastReportedFailureGeneration != diagnostics.ObjectSnapshotGeneration
					|| lastReportedFailureFingerprint
						!= diagnostics.ReflectionLayoutFingerprint)
				{
					std::cerr << "[UExplorer] Type capture failed: capture="
						<< UExplorer::Runtime::ToString(diagnostics.Error)
						<< " source=" << UExplorer::Runtime::ToString(diagnostics.SourceError)
						<< " publish=" << UExplorer::Runtime::ToString(diagnostics.PublishError)
						<< " snapshot_generation=" << diagnostics.ObjectSnapshotGeneration
						<< " reflection_fingerprint=0x" << std::hex
						<< diagnostics.ReflectionLayoutFingerprint << std::dec << "\n";
					lastReportedFailureGeneration = diagnostics.ObjectSnapshotGeneration;
					lastReportedFailureFingerprint = diagnostics.ReflectionLayoutFingerprint;
				}
				if (!DetachTypeFrameClient(std::chrono::milliseconds(5000)))
					return false;
				(void)capture->ReclaimRetired();
				if (!g_TypeSource->ReleasePreparedPlan())
					return false;
			}
			else if (diagnostics.State == UExplorer::Runtime::TypeSnapshotCaptureState::Completed)
			{
				if (!DetachTypeFrameClient(std::chrono::milliseconds(5000))
					|| !g_TypeSource->ReleasePreparedPlan())
				{
					return false;
				}
			}
		}

		const std::shared_ptr<const UExplorer::Runtime::EngineSnapshot> objects =
			g_EngineFacade->Snapshots().Current();
		const std::shared_ptr<const UExplorer::Runtime::ReflectionRuntimeSnapshot> reflection =
			g_EngineFacade->Reflection();
		if (!objects || !reflection || !reflection->Layout
			|| !reflection->IsLayoutConfigured(g_EngineFacade->ContextGeneration()))
		{
			return true;
		}
		const std::shared_ptr<const UExplorer::Runtime::TypeSnapshot> current =
			g_EngineFacade->Types().Current();
		if (current
			&& current->ObjectSnapshotGeneration() == objects->Generation
			&& current->ReflectionLayoutFingerprint() == reflection->Layout->Fingerprint()
			&& current->IsConfigured(g_EngineFacade->ContextGeneration()))
		{
			return true;
		}
		const UExplorer::Runtime::EngineSnapshotCapture* snapshotCapture =
			g_EngineFacade->SnapshotCapture();
		if (snapshotCapture
			&& IsSnapshotCaptureActive(snapshotCapture->Diagnostics().State))
		{
			return true;
		}
		const UExplorer::Runtime::ReflectionLayoutCapture* reflectionCapture =
			g_EngineFacade->ReflectionCapture();
		if (reflectionCapture
			&& IsReflectionCaptureActive(reflectionCapture->Diagnostics().State))
		{
			return true;
		}
		const std::uint64_t fingerprint = reflection->Layout->Fingerprint();
		if (objects->Generation == lastPreparationGeneration
			&& fingerprint == lastPreparationFingerprint)
		{
			return true;
		}
		lastPreparationGeneration = objects->Generation;
		lastPreparationFingerprint = fingerprint;

		const UExplorer::Runtime::TypeCandidatePreparationResult prepared =
			g_TypeSource->Prepare();
		if (!prepared.Ok())
		{
			std::cerr << "[UExplorer] Type preparation unavailable: code="
				<< UExplorer::Runtime::ToString(prepared.Error)
				<< " snapshot_generation=" << objects->Generation
				<< " reflection_fingerprint=0x" << std::hex << fingerprint
				<< std::dec << "\n";
			return prepared.Error != UExplorer::Runtime::TypeCandidatePreparationError::Busy
				&& g_TypeSource->ReleasePreparedPlan();
		}

		capture = g_EngineFacade->TypeCapture();
		if (!capture)
		{
			if (!g_EngineFacade->ConfigureTypeSnapshotCapture(*g_TypeSource))
			{
				std::cerr << "[UExplorer] Type capture ownership configuration failed.\n";
				(void)g_TypeSource->ReleasePreparedPlan();
				return false;
			}
			capture = g_EngineFacade->TypeCapture();
		}
		if (!capture)
			return false;
		if (!g_TypeFrameClientAttached)
		{
			if (!UExplorer::Runtime::GetGameThreadFrameScheduler().AttachClient(*capture))
			{
				std::cerr << "[UExplorer] Type frame-client attachment failed.\n";
				(void)g_TypeSource->ReleasePreparedPlan();
				return false;
			}
			g_TypeFrameClientAttached = true;
		}
		const UExplorer::Runtime::TypeSnapshotCaptureError requested =
			capture->RequestCapture();
		if (requested != UExplorer::Runtime::TypeSnapshotCaptureError::None)
		{
			std::cerr << "[UExplorer] Type capture request failed: "
				<< UExplorer::Runtime::ToString(requested) << "\n";
			if (IsTypeCapturePumping(capture->Diagnostics().State))
				return true;
			(void)DetachTypeFrameClient(std::chrono::milliseconds(5000));
			(void)g_TypeSource->ReleasePreparedPlan();
			return false;
		}
		return true;
	}

	bool DriveWorldDiscovery(
		std::uint64_t& lastRequestedTypeGeneration,
		std::uint64_t& lastReportedFailureGeneration)
	{
		if (!g_EngineFacade)
			return true;
		UExplorer::Runtime::WorldSnapshotCapture* capture =
			g_EngineFacade->WorldCapture();
		if (!capture)
			return true;
		const UExplorer::Runtime::WorldSnapshotCaptureDiagnostics diagnostics =
			capture->Diagnostics();
		if (diagnostics.State == UExplorer::Runtime::WorldSnapshotCaptureState::Sealing)
		{
			capture->SealForValidation();
			return true;
		}
		if (diagnostics.State == UExplorer::Runtime::WorldSnapshotCaptureState::Publishing)
		{
			capture->PublishReady();
			return true;
		}
		if (IsWorldCaptureActive(diagnostics.State))
			return true;
		if (diagnostics.State == UExplorer::Runtime::WorldSnapshotCaptureState::Failed
			&& diagnostics.RequestedGeneration != lastReportedFailureGeneration)
		{
			std::cerr << "[UExplorer] World snapshot capture failed: capture="
				<< UExplorer::Runtime::ToString(diagnostics.Error)
				<< " publish=" << UExplorer::Runtime::ToString(diagnostics.PublishError)
				<< " object_snapshot_generation=" << diagnostics.ObjectSnapshotGeneration
				<< " type_snapshot_generation=" << diagnostics.TypeSnapshotGeneration
				<< " object_index=" << diagnostics.ErrorObjectIndex << "\n";
			lastReportedFailureGeneration = diagnostics.RequestedGeneration;
		}

		const std::shared_ptr<const UExplorer::Runtime::TypeSnapshot> types =
			g_EngineFacade->Types().Current();
		if (!types || !types->IsConfigured(g_EngineFacade->ContextGeneration()))
			return true;
		const std::shared_ptr<const UExplorer::Runtime::WorldSnapshot> current =
			g_EngineFacade->Worlds().Current();
		if ((current && current->TypeSnapshotGeneration == types->Generation())
			|| lastRequestedTypeGeneration == types->Generation())
		{
			return true;
		}
		lastRequestedTypeGeneration = types->Generation();
		const UExplorer::Runtime::WorldSnapshotCaptureRequestResult requested =
			capture->RequestCapture();
		if (!requested.Ok())
		{
			std::cerr << "[UExplorer] World snapshot request unavailable: "
				<< UExplorer::Runtime::ToString(requested.Error)
				<< " type_snapshot_generation=" << types->Generation() << "\n";
		}
		return true;
	}

	void DriveProcessEventHook(
		std::uint64_t& lastAttemptedTypeGeneration,
		UExplorer::Services::ProcessEventHookError& lastReportedError,
		std::chrono::steady_clock::time_point& nextRetry)
	{
		if (!g_ProcessEventHook || !g_EngineFacade)
			return;
		const std::shared_ptr<const UExplorer::Runtime::TypeSnapshot> types =
			g_EngineFacade->Types().Current();
		if (!types)
			return;
		const UExplorer::Services::ProcessEventHookDiagnostics diagnostics =
			g_ProcessEventHook->Diagnostics();
		if (diagnostics.Installed
			&& diagnostics.CoverageComplete
			&& diagnostics.CoveredTypeSnapshotGeneration == types->Generation())
		{
			lastReportedError = UExplorer::Services::ProcessEventHookError::None;
			return;
		}

		const auto now = std::chrono::steady_clock::now();
		if (lastAttemptedTypeGeneration == types->Generation() && now < nextRetry)
			return;
		lastAttemptedTypeGeneration = types->Generation();
		nextRetry = now + std::chrono::seconds(1);
		const UExplorer::Services::ProcessEventHookResult reconciled =
			g_ProcessEventHook->Reconcile(std::chrono::milliseconds(5000));
		if (reconciled.Ok())
		{
			if (reconciled.Changed)
			{
				std::cerr << "[UExplorer] ProcessEvent hook coverage published: type_snapshot_generation="
					<< reconciled.TypeSnapshotGeneration
					<< " patched_vtables=" << reconciled.TotalPatchCount << "\n";
			}
			lastReportedError = UExplorer::Services::ProcessEventHookError::None;
			return;
		}
		if (lastReportedError != reconciled.Error)
		{
			std::cerr << "[UExplorer] ProcessEvent hook unavailable: "
				<< UExplorer::Services::ToString(reconciled.Error)
				<< " type_snapshot_generation=" << reconciled.TypeSnapshotGeneration
				<< "\n";
			lastReportedError = reconciled.Error;
		}
	}

	void ReclaimSnapshotStorage()
	{
		if (!g_EngineFacade)
			return;
		(void)g_EngineFacade->Snapshots().ReclaimRetired();
		if (UExplorer::Runtime::EngineSnapshotCapture* capture =
			g_EngineFacade->SnapshotCapture())
		{
			(void)capture->ReclaimRetired();
		}
		if (UExplorer::Runtime::TypeSnapshotCapture* capture =
			g_EngineFacade->TypeCapture())
		{
			(void)capture->ReclaimRetired();
		}
	}

	bool DetachFrameScheduling(const std::chrono::milliseconds timeout)
	{
		// Stop producing samples before waiting for the scheduler callback barrier.
		if (g_WatchScheduler
			&& !g_WatchScheduler->StopAndDrain(timeout).Ok())
		{
			return false;
		}
		if (!DetachWatchFrameClient(timeout))
			return false;
		if (!DetachWorldFrameClient(timeout))
			return false;
		if (!DetachTypeFrameClient(timeout))
			return false;
		if (!DetachReflectionFrameClient(timeout))
			return false;
		if (g_SnapshotFrameClientAttached)
		{
			UExplorer::Runtime::EngineSnapshotCapture* capture =
				g_EngineFacade ? g_EngineFacade->SnapshotCapture() : nullptr;
			if (!capture
				|| !UExplorer::Runtime::GetGameThreadFrameScheduler().DetachClient(
					*capture,
					timeout))
			{
				return false;
			}
			g_SnapshotFrameClientAttached = false;
		}

		if (g_FrameSchedulerPumpAttached)
		{
			auto& scheduler = UExplorer::Runtime::GetGameThreadFrameScheduler();
			if (!UExplorer::Runtime::GetPostRenderPumpBackend().DetachFrameClient(
				scheduler,
				timeout))
			{
				return false;
			}
			g_FrameSchedulerPumpAttached = false;
		}
		return true;
	}

	void StopFailedInitialization(HMODULE module, FILE* consoleFile, const char* code, const std::string& message)
	{
		g_Runtime.MarkFailed(code, message);
		g_Runtime.BeginStopping();
		UExplorer::Services::SetCoreCommandService(nullptr);
		if (g_DomainEventPump
			&& !g_DomainEventPump->StopAndDrain(std::chrono::milliseconds(5000)).Ok())
		{
			g_Runtime.RecordShutdownFailure(
				"DOMAIN_EVENT_PUMP_INITIALIZATION_STOP_TIMEOUT",
				"Watch/Hook event publisher did not drain after initialization failed");
			std::cerr << "[UExplorer] Domain event pump did not drain; DLL remains loaded.\n";
			if (consoleFile)
				fclose(consoleFile);
			FreeConsole();
			ExitThread(1);
		}
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
		if (g_CallBatchCoordinator
			&& !g_CallBatchCoordinator->StopAndDrain(
				std::chrono::milliseconds(5000)).Ok())
		{
			g_Runtime.RecordShutdownFailure(
				"CALL_BATCH_INITIALIZATION_STOP_TIMEOUT",
				"Call batch worker did not drain after initialization failed");
			std::cerr << "[UExplorer] Call batch worker did not drain; DLL remains loaded.\n";
			if (consoleFile)
				fclose(consoleFile);
			FreeConsole();
			ExitThread(1);
		}
		if (g_BlueprintBytecodeSource
			&& !g_BlueprintBytecodeSource->StopAndDrain(
				std::chrono::milliseconds(5000)))
		{
			g_Runtime.RecordShutdownFailure(
				"BLUEPRINT_CAPTURE_INITIALIZATION_STOP_TIMEOUT",
				"Blueprint capture work did not drain after initialization failed");
			std::cerr << "[UExplorer] Blueprint capture work did not drain; DLL remains loaded.\n";
			if (consoleFile)
				fclose(consoleFile);
			FreeConsole();
			ExitThread(1);
		}
		if (!g_Runtime.WaitForRequests(std::chrono::milliseconds(5000)))
		{
			g_Runtime.RecordShutdownFailure(
				"REQUEST_INITIALIZATION_DRAIN_TIMEOUT",
				"Core requests did not drain after initialization failed");
			std::cerr << "[UExplorer] Core requests did not drain; DLL remains loaded.\n";
			if (consoleFile)
				fclose(consoleFile);
			FreeConsole();
			ExitThread(1);
		}
		if (g_ProcessEventHook
			&& !g_ProcessEventHook->StopAndDrain(
				std::chrono::milliseconds(5000)).Ok())
		{
			g_Runtime.RecordShutdownFailure(
				"PROCESS_EVENT_HOOK_INITIALIZATION_STOP_FAILED",
				"ProcessEvent patches or callbacks did not drain after initialization failed");
			std::cerr << "[UExplorer] ProcessEvent hook did not restore; DLL remains loaded.\n";
			if (consoleFile)
				fclose(consoleFile);
			FreeConsole();
			ExitThread(1);
		}
		g_DomainEventPump.reset();
		g_PipeServer.reset();
		if (!DetachFrameScheduling(std::chrono::milliseconds(5000)))
		{
			g_Runtime.RecordShutdownFailure(
				"FRAME_SCHEDULER_INITIALIZATION_STOP_TIMEOUT",
				"Frame scheduler or one of its clients did not drain after initialization failed");
			std::cerr << "[UExplorer] Frame scheduling did not drain; DLL remains loaded.\n";
			if (consoleFile)
				fclose(consoleFile);
			FreeConsole();
			ExitThread(1);
		}
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
		g_CommandService.reset();
		g_BlueprintBytecodeSource.reset();
		g_CallBatchCommandService.reset();
		g_CallBatchCoordinator.reset();
		g_CallBatchWorker.reset();
		g_CallBatchAdapter.reset();
		g_ProcessEventHook.reset();
		g_DomainEventPump.reset();
		g_HookCommandService.reset();
		if (g_HookCollector
			&& !g_HookCollector->StopAndDrain(std::chrono::milliseconds(5000)).Ok())
		{
			g_Runtime.RecordShutdownFailure(
				"HOOK_COLLECTOR_INITIALIZATION_STOP_TIMEOUT",
				"Bounded hook collector did not drain after initialization failed");
			std::cerr << "[UExplorer] Hook collector did not drain; DLL remains loaded.\n";
			if (consoleFile)
				fclose(consoleFile);
			FreeConsole();
			ExitThread(1);
		}
		g_HookCollector.reset();
		g_DumpCommandService.reset();
		if (g_DumpCoordinator
			&& !g_DumpCoordinator->StopAndDrain(std::chrono::milliseconds(5000)).Ok())
		{
			g_Runtime.RecordShutdownFailure(
				"DUMP_INITIALIZATION_STOP_TIMEOUT",
				"Owned dump worker did not drain after initialization failed");
			std::cerr << "[UExplorer] Dump worker did not drain; DLL remains loaded.\n";
			if (consoleFile)
				fclose(consoleFile);
			FreeConsole();
			ExitThread(1);
		}
		g_DumpCoordinator.reset();
		g_DumpWorker.reset();
		g_WatchScheduler.reset();
		g_WatchSource.reset();
		if (g_EngineFacade
			&& !g_EngineFacade->Stop(std::chrono::milliseconds(5000)))
		{
			g_Runtime.RecordShutdownFailure(
				"ENGINE_FACADE_INITIALIZATION_STOP_TIMEOUT",
				"EngineFacade did not drain after initialization failed");
			std::cerr << "[UExplorer] EngineFacade did not drain; DLL remains loaded.\n";
			if (consoleFile)
				fclose(consoleFile);
			FreeConsole();
			ExitThread(1);
		}
		g_TypeSource.reset();
		g_ReflectionSource.reset();
		g_SnapshotSource.reset();
		g_EngineFacade.reset();
		g_IdentitySource.reset();
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
			g_ReflectionSource = std::make_unique<
				UExplorer::Runtime::ObjectSnapshotReflectionCandidateSource>(
					runtimeSnapshot.Context,
					*g_EngineFacade);
			g_TypeSource = std::make_unique<
				UExplorer::Runtime::ObjectSnapshotTypeCandidateSource>(
					runtimeSnapshot.Context,
					*g_EngineFacade);
			if (runtimeSnapshot.Context->HasValidatedOffset("gworld")
				&& !g_EngineFacade->ConfigureWorldSnapshotCapture())
			{
				std::cerr << "[UExplorer] World snapshot capture rejected the immutable context.\n";
			}
		}
		else
		{
			std::cerr << "[UExplorer] Object snapshot unavailable: immutable names or serial-backed object slots are not validated.\n";
		}
		g_WatchSource = std::make_unique<UExplorer::Services::ObjectPropertyWatchSampleSource>(
			g_Runtime,
			*g_EngineFacade);
		g_WatchScheduler = std::make_unique<UExplorer::Runtime::WatchScheduler>(
			runtimeSnapshot.SessionId,
			runtimeSnapshot.Context->Generation(),
			*g_WatchSource);
		if (!g_WatchScheduler->IsConfigured())
			throw std::runtime_error("Watch scheduler rejected the runtime session/context");
		g_HookCollector = std::make_unique<UExplorer::Runtime::HookEventCollector>(
			runtimeSnapshot.SessionId,
			runtimeSnapshot.Context->Generation(),
			UExplorer::Runtime::HookCollectorLimits{},
			UExplorer::Runtime::HookCollectorConfig{.Enabled = false});
		if (!g_HookCollector->IsConfigured())
			throw std::runtime_error("Hook collector rejected the runtime session/context");
		g_HookCommandService = std::make_unique<UExplorer::Services::HookCommandService>(
			runtimeSnapshot.SessionId,
			runtimeSnapshot.Context->Generation(),
			*g_HookCollector,
			UExplorer::Services::HookCommandLimits{
				.AllowPreEncodedPayload = false
			},
			&g_Runtime,
			g_EngineFacade.get(),
			&UExplorer::Runtime::GetGameThreadExecutor());
		if (!g_HookCommandService->IsConfigured())
			throw std::runtime_error("Hook command service rejected the bounded collector");
		g_ProcessEventHook = std::make_unique<
			UExplorer::Services::ProcessEventHookOwner>(
				runtimeSnapshot.Context,
				*g_EngineFacade,
				UExplorer::Runtime::GetGameThreadExecutor(),
				*g_HookCollector,
				*g_HookCommandService);
		if (!g_ProcessEventHook->IsConfigured())
			throw std::runtime_error("ProcessEvent hook owner rejected the validated context");
		const auto dumpRoot = UExplorer::Services::ResolveDefaultSnapshotDumpRoot();
		if (!dumpRoot)
			throw std::runtime_error("LocalAppData dump root could not be resolved");
		g_DumpWorker = std::make_shared<UExplorer::Services::SnapshotDumpWorker>(
			*dumpRoot);
		if (!g_DumpWorker->IsConfigured())
			throw std::runtime_error("Snapshot dump worker rejected its bounded output root");
		g_DumpCoordinator = std::make_unique<UExplorer::Runtime::DumpJobCoordinator>(
			runtimeSnapshot.SessionId,
			runtimeSnapshot.Context->Generation(),
			g_DumpWorker);
		if (!g_DumpCoordinator->IsConfigured())
			throw std::runtime_error("Dump coordinator rejected the snapshot worker");
		g_DumpCommandService = std::make_unique<UExplorer::Services::DumpCommandService>(
			g_DumpCoordinator.get());
		g_CallBatchAdapter = std::make_shared<
			UExplorer::Services::FunctionCallBatchExactInvokeAdapter>(
				g_Runtime,
				*g_EngineFacade,
				UExplorer::Runtime::GetGameThreadExecutor());
		if (!g_CallBatchAdapter->IsConfigured())
			throw std::runtime_error("Call batch exact-call adapter rejected the runtime generation");
		g_CallBatchWorker = std::make_shared<
			UExplorer::Services::FunctionCallBatchCommandWorker>(g_CallBatchAdapter);
		if (!g_CallBatchWorker->IsConfigured())
			throw std::runtime_error("Call batch worker did not retain its exact-call adapter");
		g_CallBatchCoordinator = std::make_unique<
			UExplorer::Runtime::FunctionCallBatchCoordinator>(
				runtimeSnapshot.SessionId,
				runtimeSnapshot.Context->Generation(),
				g_CallBatchWorker);
		if (!g_CallBatchCoordinator->IsConfigured())
			throw std::runtime_error("Call batch coordinator rejected the bounded worker");
		g_CallBatchCommandService = std::make_unique<
			UExplorer::Services::FunctionCallBatchCommandService>(
				g_CallBatchCoordinator.get());
		if (!g_CallBatchCommandService->IsConfigured())
			throw std::runtime_error("Call batch command service rejected the owned coordinator");
		if (runtimeSnapshot.Context->HasValidatedOffset("ufunction.script"))
		{
			g_BlueprintBytecodeSource = std::make_unique<
				UExplorer::Runtime::BlueprintBytecodeRuntimeSource>(
					runtimeSnapshot.Context,
					*g_EngineFacade,
					UExplorer::Runtime::GetGameThreadExecutor());
			if (!g_BlueprintBytecodeSource->IsConfigured())
				throw std::runtime_error("Blueprint bytecode source rejected the witnessed Script layout");
		}
		else
		{
			std::cerr << "[UExplorer] Blueprint bytecode capture unavailable: UFunction::Script lacks a high-confidence runtime witness.\n";
		}
		g_CommandService = std::make_unique<UExplorer::Services::CoreCommandService>(
			g_Runtime,
			UExplorer::Runtime::GetGameThreadExecutor(),
			*g_EngineFacade,
			g_StatusDiagnostics,
			g_ReflectionSource.get(),
			g_TypeSource.get(),
			g_WatchScheduler.get(),
			g_BlueprintBytecodeSource.get(),
			nullptr,
			g_HookCommandService.get(),
			g_DumpCommandService.get(),
			g_CallBatchCommandService.get());
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
		g_DomainEventPump = std::make_unique<UExplorer::Services::DomainEventPump>(
			*g_WatchScheduler,
			*g_HookCommandService,
			*g_PipeServer);
		if (!g_DomainEventPump->IsConfigured() || !g_DomainEventPump->Start())
			throw std::runtime_error("Domain event pump rejected the watch/hook/pipe envelope");
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
		auto& frameScheduler = UExplorer::Runtime::GetGameThreadFrameScheduler();
		if (!UExplorer::Runtime::GetPostRenderPumpBackend().AttachFrameClient(frameScheduler))
			throw std::runtime_error("PostRender frame scheduler attachment failed");
		g_FrameSchedulerPumpAttached = true;
		if (!frameScheduler.AttachClient(*g_WatchScheduler))
			throw std::runtime_error("watch scheduler frame-client attachment failed");
		g_WatchFrameClientAttached = true;
		if (UExplorer::Runtime::EngineSnapshotCapture* capture =
			g_EngineFacade ? g_EngineFacade->SnapshotCapture() : nullptr)
		{
			if (!frameScheduler.AttachClient(*capture))
				throw std::runtime_error("object snapshot frame-client attachment failed");
			g_SnapshotFrameClientAttached = true;
			const UExplorer::Runtime::SnapshotCaptureRequestResult requested =
				capture->RequestCapture();
			if (!requested.Ok())
			{
				throw std::runtime_error(
					std::string("initial object snapshot request failed: ")
					+ UExplorer::Runtime::ToString(requested.Error));
			}
		}
		if (UExplorer::Runtime::WorldSnapshotCapture* capture =
			g_EngineFacade ? g_EngineFacade->WorldCapture() : nullptr)
		{
			if (!frameScheduler.AttachClient(*capture))
				throw std::runtime_error("world snapshot frame-client attachment failed");
			g_WorldFrameClientAttached = true;
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
	std::uint64_t lastReflectionPreparationGeneration = 0;
	std::uint64_t lastReportedReflectionFailureGeneration = 0;
	std::uint64_t lastTypePreparationGeneration = 0;
	std::uint64_t lastTypePreparationFingerprint = 0;
	std::uint64_t lastReportedTypeFailureGeneration = 0;
	std::uint64_t lastReportedTypeFailureFingerprint = 0;
	std::uint64_t lastRequestedWorldTypeGeneration = 0;
	std::uint64_t lastReportedWorldFailureGeneration = 0;
	std::uint64_t lastHookAttemptedTypeGeneration = 0;
	UExplorer::Services::ProcessEventHookError lastReportedHookError =
		UExplorer::Services::ProcessEventHookError::None;
	auto nextHookRetry = std::chrono::steady_clock::now();
	while (startupReady && g_Running.load())
	{
		ReclaimSnapshotStorage();
		if (!DriveReflectionDiscovery(
			lastReflectionPreparationGeneration,
			lastReportedReflectionFailureGeneration))
		{
			std::cerr << "[UExplorer] Reflection lifecycle could not release its frame/snapshot ownership.\n";
			g_Running.store(false, std::memory_order_release);
			break;
		}
		if (!DriveTypeDiscovery(
			lastTypePreparationGeneration,
			lastTypePreparationFingerprint,
			lastReportedTypeFailureGeneration,
			lastReportedTypeFailureFingerprint))
		{
			std::cerr << "[UExplorer] Type lifecycle could not release its frame/snapshot ownership.\n";
			g_Running.store(false, std::memory_order_release);
			break;
		}
		if (!DriveWorldDiscovery(
			lastRequestedWorldTypeGeneration,
			lastReportedWorldFailureGeneration))
		{
			std::cerr << "[UExplorer] World snapshot lifecycle failed.\n";
			g_Running.store(false, std::memory_order_release);
			break;
		}
		DriveProcessEventHook(
			lastHookAttemptedTypeGeneration,
			lastReportedHookError,
			nextHookRetry);
		RefreshRuntimeCapabilities();
		if (!EnsurePipeAdmissions())
		{
			std::cerr << "[UExplorer] Named Pipe listener/admission contract failed.\n";
			g_Running.store(false, std::memory_order_release);
			break;
		}
		const auto now = std::chrono::steady_clock::now();
		if (g_SnapshotFrameClientAttached
			&& !ReflectionCaptureBlocksSnapshotRefresh()
			&& !TypeCaptureBlocksSnapshotRefresh()
			&& !WorldCaptureBlocksSnapshotRefresh()
			&& now >= nextSnapshotRefresh)
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
	bool domainEventPumpStopped = true;
	bool callBatchStopped = true;
	bool dumpStopped = true;
	bool blueprintCaptureStopped = true;
	bool processEventHookStopped = true;
	bool hooksStopped = true;
	bool worldFrameStopped = true;
	bool watchFrameStopped = true;
	bool typeFrameStopped = true;
	bool reflectionFrameStopped = true;
	bool snapshotFrameStopped = true;
	bool frameSchedulerStopped = true;
	bool hookCollectorStopped = true;
	bool requestsDrained = false;
	UExplorer::Runtime::ShutdownCoordinator shutdown;
	shutdown.AddStage("named_pipe", [&] {
		pipeStopped = !g_PipeServer
			|| g_PipeServer->Stop(std::chrono::milliseconds(5000));
		return pipeStopped;
	});
	shutdown.AddStage("call_batch", [&] {
		if (!pipeStopped)
			return false;
		callBatchStopped = !g_CallBatchCoordinator
			|| g_CallBatchCoordinator->StopAndDrain(
				std::chrono::milliseconds(5000)).Ok();
		return callBatchStopped;
	});
	shutdown.AddStage("dump_jobs", [&] {
		if (!pipeStopped || !callBatchStopped)
			return false;
		dumpStopped = !g_DumpCoordinator
			|| g_DumpCoordinator->StopAndDrain(
				std::chrono::milliseconds(5000)).Ok();
		return dumpStopped;
	});
	shutdown.AddStage("blueprint_capture", [&] {
		if (!pipeStopped || !callBatchStopped || !dumpStopped)
			return false;
		blueprintCaptureStopped = !g_BlueprintBytecodeSource
			|| g_BlueprintBytecodeSource->StopAndDrain(
				std::chrono::milliseconds(5000));
		return blueprintCaptureStopped;
	});
	shutdown.AddStage("runtime_requests", [&] {
		if (!pipeStopped || !callBatchStopped || !dumpStopped || !blueprintCaptureStopped)
			return false;
		requestsDrained = g_Runtime.WaitForRequests(std::chrono::milliseconds(5000));
		return requestsDrained;
	});
	shutdown.AddStage("process_event_hook", [&] {
		if (!pipeStopped || !requestsDrained)
			return false;
		processEventHookStopped = !g_ProcessEventHook
			|| g_ProcessEventHook->StopAndDrain(
				std::chrono::milliseconds(5000)).Ok();
		return processEventHookStopped;
	});
	shutdown.AddStage("world_frame_client", [&] {
		if (!pipeStopped || !requestsDrained || !processEventHookStopped)
			return false;
		worldFrameStopped = DetachWorldFrameClient(
			std::chrono::milliseconds(5000));
		return worldFrameStopped;
	});
	shutdown.AddStage("watch_frame_client", [&] {
		if (!pipeStopped || !requestsDrained || !processEventHookStopped)
			return false;
		if (g_WatchScheduler)
		{
			watchFrameStopped = g_WatchScheduler->StopAndDrain(
				std::chrono::milliseconds(5000)).Ok();
			if (!watchFrameStopped)
				return false;
		}
		watchFrameStopped = DetachWatchFrameClient(
			std::chrono::milliseconds(5000));
		return watchFrameStopped;
	});
	shutdown.AddStage("hook_collector", [&] {
		if (!pipeStopped || !requestsDrained || !processEventHookStopped || !watchFrameStopped)
			return false;
		hookCollectorStopped = !g_HookCollector
			|| g_HookCollector->StopAndDrain(std::chrono::milliseconds(5000)).Ok();
		return hookCollectorStopped;
	});
	shutdown.AddStage("domain_event_pump", [&] {
		if (!pipeStopped
			|| !requestsDrained
			|| !processEventHookStopped
			|| !watchFrameStopped
			|| !hookCollectorStopped)
		{
			return false;
		}
		domainEventPumpStopped = !g_DomainEventPump
			|| g_DomainEventPump->StopAndDrain(std::chrono::milliseconds(5000)).Ok();
		return domainEventPumpStopped;
	});
	shutdown.AddStage("type_frame_client", [&] {
		if (!pipeStopped || !requestsDrained || !processEventHookStopped)
			return false;
		typeFrameStopped = DetachTypeFrameClient(
			std::chrono::milliseconds(5000));
		return typeFrameStopped;
	});
	shutdown.AddStage("reflection_frame_client", [&] {
		if (!pipeStopped || !requestsDrained || !processEventHookStopped)
			return false;
		reflectionFrameStopped = DetachReflectionFrameClient(
			std::chrono::milliseconds(5000));
		return reflectionFrameStopped;
	});
	shutdown.AddStage("snapshot_frame_client", [&] {
		if (!pipeStopped || !requestsDrained || !processEventHookStopped)
			return false;
		if (!g_SnapshotFrameClientAttached)
			return true;
		UExplorer::Runtime::EngineSnapshotCapture* capture =
			g_EngineFacade ? g_EngineFacade->SnapshotCapture() : nullptr;
		if (!capture)
			return false;
		snapshotFrameStopped = UExplorer::Runtime::GetGameThreadFrameScheduler().DetachClient(
			*capture,
			std::chrono::milliseconds(5000));
		if (snapshotFrameStopped)
			g_SnapshotFrameClientAttached = false;
		return snapshotFrameStopped;
	});
	shutdown.AddStage("frame_scheduler", [&] {
		if (!pipeStopped || !requestsDrained || !processEventHookStopped)
			return false;
		if (g_FrameSchedulerPumpAttached)
		{
			auto& scheduler = UExplorer::Runtime::GetGameThreadFrameScheduler();
			frameSchedulerStopped = UExplorer::Runtime::GetPostRenderPumpBackend().DetachFrameClient(
				scheduler,
				std::chrono::milliseconds(5000));
			if (frameSchedulerStopped)
				g_FrameSchedulerPumpAttached = false;
		}
		return frameSchedulerStopped;
	});
	shutdown.AddStage("post_render_hook", [&] {
		if (!pipeStopped || !requestsDrained || !processEventHookStopped)
			return false;
		// The game-thread pump remains active until every frame client has drained.
		hooksStopped = !g_PostRenderHook
			|| g_PostRenderHook->Stop(std::chrono::milliseconds(5000));
		return hooksStopped;
	});
	shutdown.AddStage("engine_facade", [&] {
		return pipeStopped
			&& callBatchStopped
			&& dumpStopped
			&& blueprintCaptureStopped
			&& processEventHookStopped
			&& requestsDrained
			&& worldFrameStopped
			&& watchFrameStopped
			&& typeFrameStopped
			&& reflectionFrameStopped
			&& snapshotFrameStopped
			&& frameSchedulerStopped
			&& hookCollectorStopped
			&& domainEventPumpStopped
			&& hooksStopped
			&& (!g_EngineFacade
				|| g_EngineFacade->Stop(std::chrono::milliseconds(5000)));
	});
	const UExplorer::Runtime::ShutdownReport shutdownReport = shutdown.Run();
	unloadSafe = unloadSafe && shutdownReport.SafeToUnload;
	if (domainEventPumpStopped)
		g_DomainEventPump.reset();
	if (pipeStopped && domainEventPumpStopped)
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
	g_BlueprintBytecodeSource.reset();
	g_CallBatchCommandService.reset();
	g_CallBatchCoordinator.reset();
	g_CallBatchWorker.reset();
	g_CallBatchAdapter.reset();
	g_ProcessEventHook.reset();
	g_DomainEventPump.reset();
	g_HookCommandService.reset();
	g_HookCollector.reset();
	g_DumpCommandService.reset();
	g_DumpCoordinator.reset();
	g_DumpWorker.reset();
	g_WatchScheduler.reset();
	g_WatchSource.reset();
	g_TypeSource.reset();
	g_ReflectionSource.reset();
	g_SnapshotSource.reset();
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
		DisableThreadLibraryCalls(hModule);
		if (HANDLE thread = CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr))
			CloseHandle(thread);
		else
			return FALSE;
		break;
	}
	return TRUE;
}
