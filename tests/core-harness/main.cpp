#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include "IPC/Protocol.h"
#include "Runtime/BoundedQueue.h"
#include "Runtime/CallbackBarrier.h"
#include "Runtime/CoreCapabilities.h"
#include "Runtime/CoreRuntime.h"
#include "Runtime/FUObjectItemLayout.h"
#include "Runtime/GameThreadExecutor.h"
#include "Runtime/ObjectHandle.h"
#include "Runtime/SafeMemory.h"
#include "Runtime/ShutdownCoordinator.h"
#include "Runtime/VTableHook.h"
#include "API/GameThreadQueue.h"
#include "Generator/Public/Generators/UsmapContainer.h"
#include "Server/HttpServer.h"

#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <future>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace
{
	using namespace UExplorer::IPC;
	std::atomic<int> g_ProcessEventCalls{ 0 };
	std::atomic<bool> g_BlockProcessEvent{ false };
	std::atomic<bool> g_ProcessEventEntered{ false };

	void Require(const bool condition, const char* message)
	{
		if (!condition)
			throw std::runtime_error(message);
	}

	void WaitUntil(const std::function<bool()>& predicate, const char* message)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!predicate())
		{
			if (std::chrono::steady_clock::now() >= deadline)
				throw std::runtime_error(message);
			std::this_thread::yield();
		}
	}

	void FakeProcessEvent(void*, void*, void* params)
	{
		g_ProcessEventCalls.fetch_add(1);
		g_ProcessEventEntered.store(true);
		while (g_BlockProcessEvent.load())
			std::this_thread::yield();
		if (params)
			*static_cast<std::uint8_t*>(params) = 42;
	}

	void FaultingProcessEvent(void*, void*, void*)
	{
		RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
	}

	class OwnedProbeWork final : public UExplorer::Runtime::IGameThreadWork
	{
	public:
		explicit OwnedProbeWork(const int input, const bool shouldThrow = false)
			: Input(input), ShouldThrow(shouldThrow)
		{
		}

		bool Execute() override
		{
			ExecutionThreadId = GetCurrentThreadId();
			if (ShouldThrow)
				throw std::runtime_error("generic game-thread work failure");
			Output = Input * 2;
			return true;
		}

		int Input = 0;
		int Output = 0;
		bool ShouldThrow = false;
		std::uint32_t ExecutionThreadId = 0;
	};

	std::string ReadText(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary);
		if (!stream)
			throw std::runtime_error("Unable to open fixture: " + path.string());
		return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
	}

	std::vector<std::uint8_t> ReadHex(const std::filesystem::path& path)
	{
		const auto input = ReadText(path);
		std::string compact;
		for (const unsigned char value : input)
		{
			if (!std::isspace(value))
				compact.push_back(static_cast<char>(value));
		}
		Require(compact.size() % 2 == 0, "Hex fixture has an odd number of digits");

		std::vector<std::uint8_t> output;
		output.reserve(compact.size() / 2);
		for (std::size_t index = 0; index < compact.size(); index += 2)
		{
			const auto pair = compact.substr(index, 2);
			std::size_t parsed = 0;
			const auto value = std::stoul(pair, &parsed, 16);
			Require(parsed == pair.size(), "Hex fixture contains an invalid digit");
			output.push_back(static_cast<std::uint8_t>(value));
		}
		return output;
	}

	void TestGoldenHello(const std::filesystem::path& fixtureDirectory)
	{
		const auto encoded = ReadHex(fixtureDirectory / "hello.frame.hex");
		auto payload = ReadText(fixtureDirectory / "hello.json");
		while (!payload.empty() && (payload.back() == '\r' || payload.back() == '\n'))
			payload.pop_back();

		FrameDecoder decoder;
		std::vector<Frame> frames;
		for (const auto byte : encoded)
		{
			const auto batch = decoder.Push(std::span<const std::uint8_t>(&byte, 1));
			Require(batch.Error == ProtocolError::None, "Golden frame failed during partial decode");
			frames.insert(frames.end(), batch.Frames.begin(), batch.Frames.end());
		}

		Require(frames.size() == 1, "Golden stream must decode exactly one frame");
		Require(frames[0].Header.Kind == FrameKind::Hello, "Golden frame kind mismatch");
		Require(frames[0].Header.RequestId == 1, "Golden request ID mismatch");
		const std::string decodedPayload(frames[0].Payload.begin(), frames[0].Payload.end());
		Require(decodedPayload == payload, "Golden payload mismatch");
		Require(decoder.BufferedBytes() == 0, "Decoder retained bytes after complete frame");
	}

	void TestMultipleFrames()
	{
		const std::array<std::uint8_t, 2> pingPayload{ 'o', 'k' };
		std::vector<std::uint8_t> ping;
		std::vector<std::uint8_t> pong;
		Require(EncodeFrame(FrameKind::Ping, 7, pingPayload, ping) == ProtocolError::None, "Ping encode failed");
		Require(EncodeFrame(FrameKind::Pong, 7, std::span<const std::uint8_t>{}, pong) == ProtocolError::None, "Pong encode failed");
		ping.insert(ping.end(), pong.begin(), pong.end());

		FrameDecoder decoder;
		const auto batch = decoder.Push(ping);
		Require(batch.Error == ProtocolError::None, "Combined frames failed to decode");
		Require(batch.Frames.size() == 2, "Combined stream did not produce two frames");
		Require(batch.Frames[0].Header.Kind == FrameKind::Ping, "First combined frame is not Ping");
		Require(batch.Frames[1].Header.Kind == FrameKind::Pong, "Second combined frame is not Pong");
	}

	void TestTerminalErrors()
	{
		std::vector<std::uint8_t> frame;
		Require(EncodeFrame(FrameKind::Request, 9, std::span<const std::uint8_t>{}, frame) == ProtocolError::None, "Request encode failed");

		auto invalidMagic = frame;
		invalidMagic[0] = 'X';
		FrameDecoder magicDecoder;
		Require(magicDecoder.Push(invalidMagic).Error == ProtocolError::InvalidMagic, "Invalid magic was accepted");

		auto invalidMajor = frame;
		invalidMajor[4] = 2;
		FrameDecoder versionDecoder;
		Require(versionDecoder.Push(invalidMajor).Error == ProtocolError::UnsupportedMajor, "Protocol major fallback occurred");

		auto oversized = frame;
		Detail::WriteU32(oversized.data() + 12, MaxPayloadSize + 1);
		FrameDecoder sizeDecoder;
		Require(sizeDecoder.Push(oversized).Error == ProtocolError::PayloadTooLarge, "Oversized payload was accepted");
	}

	std::uint32_t ReadU32LittleEndian(const std::vector<std::uint8_t>& bytes, const std::size_t offset)
	{
		Require(offset <= bytes.size() && bytes.size() - offset >= 4, "Independent USMAP consumer read past input");
		return static_cast<std::uint32_t>(bytes[offset])
			| (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
			| (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
			| (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
	}

	void TestUsmapContainer(const std::filesystem::path& fixtureDirectory)
	{
		const std::vector<std::uint8_t> payload(12, 0);
		std::ostringstream output(std::ios::binary | std::ios::out);
		UExplorer::Usmap::WriteUncompressed(output, payload);
		const std::string encodedText = output.str();
		const std::vector<std::uint8_t> encoded(encodedText.begin(), encodedText.end());
		Require(encoded == ReadHex(fixtureDirectory / "usmap-none.hex"), "USMAP writer changed from the golden container");

		// Parse independently from the production writer and validate a minimal payload.
		Require(encoded.size() == 28, "Independent USMAP consumer rejected container length");
		Require(encoded[0] == 0xC4 && encoded[1] == 0x30, "Independent USMAP consumer rejected magic");
		Require(encoded[2] == 4, "Independent USMAP consumer rejected version");
		Require(ReadU32LittleEndian(encoded, 3) == 0, "Independent USMAP consumer found unexpected package versioning");
		Require(encoded[7] == 0, "Independent USMAP consumer found an unsupported compression method");
		Require(ReadU32LittleEndian(encoded, 8) == 12, "Independent USMAP consumer rejected compressed size");
		Require(ReadU32LittleEndian(encoded, 12) == 12, "Independent USMAP consumer rejected uncompressed size");
		Require(ReadU32LittleEndian(encoded, 16) == 0, "Independent USMAP consumer rejected name count");
		Require(ReadU32LittleEndian(encoded, 20) == 0, "Independent USMAP consumer rejected enum count");
		Require(ReadU32LittleEndian(encoded, 24) == 0, "Independent USMAP consumer rejected struct count");
	}

	UExplorer::Runtime::OffsetReport ValidatedOffset(
		std::string name,
		const std::int64_t value,
		const bool required = false)
	{
		return {
			.Name = std::move(name),
			.Value = value,
			.Required = required,
			.State = UExplorer::Runtime::ValidationState::Validated,
			.Source = "harness",
			.Checks = {"fixture"}
		};
	}

	std::shared_ptr<const UExplorer::Runtime::EngineContext> MakeEngineContext(
		const std::uint64_t generation = 1)
	{
		UExplorer::Runtime::EngineContextBuilder builder(generation);
		builder.SetIdentity(0x140000000, 0x140100000, 4242, 100, "FixtureGame", "5.4");
		builder.SetProfile({.UsesFProperty = true, .UsesLargeWorldCoordinates = true});
		builder.AddOffset(ValidatedOffset("gobjects", 0x100000, true));
		builder.AddOffset(ValidatedOffset("gworld", 0x200000));
		builder.AddOffset(ValidatedOffset("process_event.index", 0x4C, true));
		builder.AddOffset(ValidatedOffset("process_event.offset", 0x300000, true));
		return builder.Build();
	}

	void TestEngineContextAndCapabilities()
	{
		using namespace UExplorer::Runtime;

		const auto context = MakeEngineContext();
		Require(context->Generation() == 1, "Engine context generation changed");
		Require(context->HasValidatedOffset("gobjects"), "Validated offset was not published");
		Require(context->Profile().UsesFProperty, "Immutable engine profile was not published");

		bool missingRequiredRejected = false;
		try
		{
			EngineContextBuilder invalid(2);
			invalid.SetIdentity(0x140000000, 0x140100000, 4242, 1, {}, {});
			invalid.AddOffset({
				.Name = "critical",
				.Value = -1,
				.Required = true,
				.State = ValidationState::Missing
			});
			(void)invalid.Build();
		}
		catch (const std::runtime_error&)
		{
			missingRequiredRejected = true;
		}
		Require(missingRequiredRejected, "Missing critical offset produced an EngineContext");

		RuntimeProbes probes;
		probes.GameThreadExecutorEnabled = true;
		probes.GameThreadPumpObserved = true;
		probes.GameThreadPumpThreadStable = true;
		probes.GameThreadPumpActive = true;
		probes.SafeMemoryEnabled = true;
		probes.ObjectIdentitySourceEnabled = true;
		probes.ObjectHandleValidationEnabled = true;
		const auto withoutPipe = BuildCoreCapabilities(*context, probes);
		Require(!withoutPipe->IsAvailable("transport.named_pipe"), "Missing pipe listener was advertised");
		Require(withoutPipe->IsAvailable("call.invoke"), "Validated call dependencies were rejected");
		RuntimeProbes missingIdentitySource = probes;
		missingIdentitySource.ObjectIdentitySourceEnabled = false;
		const auto withoutIdentitySource = BuildCoreCapabilities(*context, missingIdentitySource);
		Require(
			!withoutIdentitySource->IsAvailable("objects.handles")
				&& !withoutIdentitySource->IsAvailable("call.invoke"),
			"Handle/call capability ignored the production identity source dependency");
		RuntimeProbes stalledProbes = probes;
		stalledProbes.GameThreadPumpActive = false;
		const auto stalled = BuildCoreCapabilities(*context, stalledProbes);
		const CapabilityStatus* stalledGameThread = stalled->Find("game_thread.executor");
		Require(
			stalledGameThread && stalledGameThread->ReasonCode == "GAME_THREAD_PUMP_STALLED",
			"Stalled game-thread pump remained available");

		probes.NamedPipeListening = true;
		const auto withPipe = BuildCoreCapabilities(*context, probes);
		for (const std::string& required : RequiredReadyCapabilities())
			Require(withPipe->IsAvailable(required), "A required ready capability is unavailable");

		bool cycleRejected = false;
		try
		{
			CapabilityRegistryBuilder cyclic;
			cyclic.Define("a", true, {}, {}, {"b"});
			cyclic.Define("b", true, {}, {}, {"a"});
			(void)cyclic.Build(1);
		}
		catch (const std::invalid_argument&)
		{
			cycleRejected = true;
		}
		Require(cycleRejected, "Capability dependency cycle was accepted");
	}

	void TestCoreRuntimeStateAndShutdown()
	{
		using namespace UExplorer::Runtime;

		CoreRuntime runtime;
		Require(runtime.BeginInitialize(), "CoreRuntime rejected Created -> Initializing");
		const auto context = MakeEngineContext();
		Require(runtime.PublishContext(context), "CoreRuntime rejected its first immutable context");

		RuntimeProbes probes;
		probes.GameThreadExecutorEnabled = true;
		probes.GameThreadPumpObserved = true;
		probes.GameThreadPumpThreadStable = true;
		probes.GameThreadPumpActive = true;
		probes.SafeMemoryEnabled = true;
		probes.ObjectIdentitySourceEnabled = true;
		probes.ObjectHandleValidationEnabled = true;
		Require(
			runtime.PublishCapabilities(BuildCoreCapabilities(*context, probes)),
			"CoreRuntime rejected capability publication");
		std::vector<std::string> blockers;
		Require(
			!runtime.TryMarkReady(RequiredReadyCapabilities(), &blockers) && !blockers.empty(),
			"CoreRuntime became Ready without its pipe listener");

		probes.NamedPipeListening = true;
		Require(
			runtime.PublishCapabilities(BuildCoreCapabilities(*context, probes)),
			"CoreRuntime rejected refreshed capabilities");
		Require(
			runtime.TryMarkReady(RequiredReadyCapabilities()),
			"CoreRuntime did not become Ready with all required capabilities");
		RuntimeProbes stalledReadyProbes = probes;
		stalledReadyProbes.GameThreadPumpActive = false;
		Require(
			runtime.PublishCapabilities(BuildCoreCapabilities(*context, stalledReadyProbes)),
			"Ready CoreRuntime rejected capability refresh");
		Require(!runtime.Snapshot().IsReady(), "Required capability loss left readiness true");
		std::string admissionError;
		Require(
			!runtime.TryAcquireRequest(&admissionError).has_value() && admissionError == "CORE_NOT_READY",
			"Degraded readiness admitted new work");
		Require(
			runtime.PublishCapabilities(BuildCoreCapabilities(*context, probes)),
			"CoreRuntime rejected capability recovery");
		Require(runtime.Snapshot().IsReady(), "Recovered required capabilities did not restore readiness");

		auto lease = runtime.TryAcquireRequest(&admissionError);
		Require(lease.has_value(), "Ready CoreRuntime rejected a request");
		Require(runtime.BeginStopping(), "CoreRuntime rejected Ready -> Stopping");
		Require(
			!runtime.TryAcquireRequest(&admissionError).has_value() && admissionError == "CORE_STOPPING",
			"Stopping CoreRuntime admitted new work");
		Require(
			!runtime.WaitForRequests(std::chrono::milliseconds(1)),
			"CoreRuntime did not wait for an owned request lease");
		lease.reset();
		Require(runtime.WaitForRequests(std::chrono::milliseconds(100)), "Request lease did not drain");
		Require(runtime.MarkStopped(), "CoreRuntime rejected Stopping -> Stopped");
		Require(runtime.Snapshot().State == CoreState::Stopped, "CoreRuntime terminal state changed");

		std::vector<std::string> order;
		ShutdownCoordinator shutdown;
		shutdown.AddStage("transport", [&] { order.push_back("transport"); return true; });
		shutdown.AddStage("hooks", [&] { order.push_back("hooks"); return false; });
		shutdown.AddStage("dump", [&]() -> bool {
			order.push_back("dump");
			throw std::runtime_error("fixture failure");
		});
		const ShutdownReport first = shutdown.Run();
		const ShutdownReport second = shutdown.Run();
		Require(!first.SafeToUnload, "Failed shutdown stage allowed unload");
		Require(first.Stages.size() == 3, "Shutdown did not report every owned stage");
		Require(order == std::vector<std::string>({"transport", "hooks", "dump"}), "Shutdown stage order changed");
		Require(second.Stages.size() == first.Stages.size() && order.size() == 3, "Shutdown coordinator ran twice");
	}

	class FakeHandleIdentitySource final : public UExplorer::Runtime::IHandleIdentitySource
	{
	public:
		bool Available = true;
		bool ThrowOnRead = false;
		std::unordered_map<std::int32_t, UExplorer::Runtime::ObjectIdentity> Objects;
		std::unordered_map<std::int32_t, UExplorer::Runtime::FunctionIdentity> Functions;

		bool TryReadObject(
			const std::int32_t index,
			UExplorer::Runtime::ObjectIdentity& identity) override
		{
			if (ThrowOnRead)
				throw std::runtime_error("fixture identity source failure");
			const auto found = Objects.find(index);
			if (!Available || found == Objects.end())
				return false;
			identity = found->second;
			return true;
		}

		bool TryReadFunction(
			const std::int32_t index,
			UExplorer::Runtime::FunctionIdentity& identity) override
		{
			if (ThrowOnRead)
				throw std::runtime_error("fixture identity source failure");
			const auto found = Functions.find(index);
			if (!Available || found == Functions.end())
				return false;
			identity = found->second;
			return true;
		}
	};

	void TestStableObjectAndFunctionHandles()
	{
		using namespace UExplorer::Runtime;

		FakeHandleIdentitySource source;
		source.Objects.emplace(7, ObjectIdentity{
			.Index = 7,
			.SerialNumber = 101,
			.Address = 0x1000,
			.ClassFingerprint = 0xA001
		});
		ObjectHandleService service("fixture-session", 42, source);
		const ObjectHandleResult issued = service.IssueObject(7);
		Require(issued.Ok(), "Stable object handle was not issued from a complete identity");
		Require(service.ValidateObject(issued.Value).Ok(), "Fresh object handle did not validate");

		ObjectHandle staleSession = issued.Value;
		staleSession.SessionId = "old-session";
		Require(
			service.ValidateObject(staleSession).Error == HandleError::SessionMismatch,
			"Object handle crossed a session boundary");
		ObjectHandle staleGeneration = issued.Value;
		staleGeneration.ContextGeneration = 41;
		Require(
			service.ValidateObject(staleGeneration).Error == HandleError::ContextGenerationMismatch,
			"Object handle crossed an EngineContext generation");

		source.Objects.at(7).SerialNumber = 102;
		Require(
			service.ValidateObject(issued.Value).Error == HandleError::SerialMismatch,
			"Recycled object slot retained a valid handle");
		source.Objects.at(7).SerialNumber = 101;
		source.Objects.at(7).Address = 0x1100;
		Require(
			service.ValidateObject(issued.Value).Error == HandleError::AddressMismatch,
			"Object address change retained a valid handle");
		source.Objects.at(7).Address = 0x1000;
		source.Objects.at(7).ClassFingerprint = 0xA002;
		Require(
			service.ValidateObject(issued.Value).Error == HandleError::ClassFingerprintMismatch,
			"Object class change retained a valid handle");
		source.Objects.at(7).ClassFingerprint = 0xA001;

		source.Objects.emplace(8, ObjectIdentity{
			.Index = 8,
			.SerialNumber = 0,
			.Address = 0x2000,
			.ClassFingerprint = 0xA001
		});
		Require(
			service.IssueObject(8).Error == HandleError::SerialUnavailable,
			"Object handle silently fell back when no serial was available");

		source.Functions.emplace(100, FunctionIdentity{
			.Function = {
				.Index = 100,
				.SerialNumber = 301,
				.Address = 0x5000,
				.ClassFingerprint = 0xF001
			},
			.Owner = {
				.Index = 200,
				.SerialNumber = 401,
				.Address = 0x6000,
				.ClassFingerprint = 0xC001
			},
			.FullPath = "Function fname:10:0.fname:20:0",
			.SignatureFingerprint = 0x5151
		});
		source.Functions.at(100).FullPath = "/Script/Fixture.Owner:Function";
		Require(
			service.IssueFunction(100).Error == HandleError::FunctionPathMismatch,
			"Non-canonical display path became a function execution identity");
		source.Functions.at(100).FullPath = "Function fname:10:0.fname:20:0";
		const FunctionHandleResult function = service.IssueFunction(100);
		Require(function.Ok(), "Stable function handle was not issued");
		Require(service.ValidateFunction(function.Value).Ok(), "Fresh function handle did not validate");

		source.Functions.at(100).Owner.SerialNumber = 402;
		Require(
			service.ValidateFunction(function.Value).Error == HandleError::FunctionOwnerMismatch,
			"Function handle ignored owner recycling");
		source.Functions.at(100).Owner.SerialNumber = 401;
		source.Functions.at(100).FullPath = "Function fname:10:0.fname:21:0";
		Require(
			service.ValidateFunction(function.Value).Error == HandleError::FunctionPathMismatch,
			"Function handle ignored a path change");
		source.Functions.at(100).FullPath = "Function fname:10:0.fname:20:0";
		source.Functions.at(100).SignatureFingerprint = 0x5252;
		Require(
			service.ValidateFunction(function.Value).Error == HandleError::FunctionSignatureMismatch,
			"Function handle ignored a signature change");
		source.Functions.at(100).SignatureFingerprint = 0x5151;
		source.Available = false;
		Require(
			service.ValidateFunction(function.Value).Error == HandleError::IdentityUnavailable,
			"Unavailable identity source was treated as a valid function");
		source.Available = true;
		source.ThrowOnRead = true;
		Require(
			service.ValidateObject(issued.Value).Error == HandleError::IdentityUnavailable,
			"Identity source exception escaped the handle boundary");

		ObjectHandleService invalidService("", 0, source);
		Require(
			invalidService.IssueObject(7).Error == HandleError::InvalidService,
			"Unconfigured handle service issued a handle");
		Require(
			std::string(ToString(HandleError::SerialMismatch)) == "HANDLE_SERIAL_MISMATCH",
			"Stable handle error code changed");
	}

	void TestHookOwnershipAndCallbackDrain()
	{
		using namespace UExplorer::Runtime;

		int originalTarget = 1;
		int replacementTarget = 2;
		int externalTarget = 3;
		void* slot = &originalTarget;
		{
			VTableHookInstallResult installed =
				VTableHookToken::Install(&slot, &replacementTarget, &originalTarget);
			Require(installed.Ok(), "VTable hook token did not install");
			Require(slot == &replacementTarget && installed.Token->IsActive(), "VTable hook token lost ownership");
			Require(installed.Token->Disable() && slot == &originalTarget, "VTable hook token did not restore");
			Require(installed.Token->Enable() && slot == &replacementTarget, "VTable hook token did not re-enable");
			slot = &externalTarget;
			Require(
				installed.Token->Disable() && slot == &externalTarget && installed.Token->SafeToUnload(),
				"VTable hook token overwrote an external replacement");
		}

		slot = &originalTarget;
		{
			VTableHookInstallResult scoped =
				VTableHookToken::Install(&slot, &replacementTarget, &originalTarget);
			Require(scoped.Ok() && slot == &replacementTarget, "Scoped VTable hook setup failed");
		}
		Require(slot == &originalTarget, "VTable hook RAII destructor did not restore its slot");
		Require(
			VTableHookToken::Install(&slot, &replacementTarget, &externalTarget).Error
				== VTableHookError::OriginalMismatch,
			"VTable hook ignored its expected original function");

		SYSTEM_INFO systemInfo{};
		GetSystemInfo(&systemInfo);
		void* page = VirtualAlloc(
			nullptr,
			systemInfo.dwPageSize,
			MEM_RESERVE | MEM_COMMIT,
			PAGE_READWRITE);
		Require(page != nullptr, "VTable hook failure fixture allocation failed");
		struct HookPageGuard
		{
			void* Address;
			~HookPageGuard() { if (Address) VirtualFree(Address, 0, MEM_RELEASE); }
		} pageGuard{page};
		auto* protectedSlot = static_cast<void**>(page);
		*protectedSlot = &originalTarget;
		VTableHookInstallResult protectedHook =
			VTableHookToken::Install(protectedSlot, &replacementTarget, &originalTarget);
		Require(protectedHook.Ok(), "Protected VTable hook fixture did not install");
		DWORD oldProtection = 0;
		Require(
			VirtualProtect(page, systemInfo.dwPageSize, PAGE_NOACCESS, &oldProtection) != FALSE,
			"VTable hook restore-failure fixture setup failed");
		Require(
			!protectedHook.Token->Disable() && protectedHook.Token->IsActive(),
			"Failed VTable restore discarded hook ownership");
		DWORD ignoredProtection = 0;
		Require(
			VirtualProtect(page, systemInfo.dwPageSize, PAGE_READWRITE, &ignoredProtection) != FALSE,
			"VTable hook restore-failure fixture cleanup failed");
		Require(
			protectedHook.Token->Disable() && protectedHook.Token->SafeToUnload()
				&& *protectedSlot == &originalTarget,
			"VTable hook restore was not retryable");

		CallbackBarrier callbacks;
		auto active = callbacks.Enter();
		Require(active.OwnedWorkAllowed(), "Active callback was rejected before shutdown");
		callbacks.BeginStopping();
		auto late = callbacks.Enter();
		Require(!late.OwnedWorkAllowed(), "Post-stop callback was allowed to run owned work");
		Require(
			!callbacks.WaitForDrain(std::chrono::milliseconds(1)),
			"Callback barrier drained while callbacks were still active");
		active = {};
		late = {};
		Require(
			callbacks.WaitForDrain(std::chrono::milliseconds(100), std::chrono::milliseconds(1)),
			"Callback barrier did not observe a quiet drained interval");
		Require(callbacks.Reset(), "Drained callback barrier did not reset");
		auto restarted = callbacks.Enter();
		Require(restarted.OwnedWorkAllowed(), "Reset callback barrier remained stopped");
	}

	void TestFUObjectItemIdentityLayout()
	{
		using namespace UExplorer::Runtime;

		std::vector<ObjectItemLayoutSample> samples;
		for (std::int32_t index = 0; index < 16; ++index)
		{
			samples.push_back({
				.SlotIndex = index,
				.InternalIndex = index,
				.ObjectAddress = 0x100000 + static_cast<std::uintptr_t>(index) * 0x100,
				.ClusterRootIndex = -1,
				.SerialNumber = index + 1,
				.Stable = true
			});
		}

		const ObjectItemLayoutValidation valid =
			ValidateEpic64ObjectItemLayoutV1(0x18, 0, 1024, samples);
		Require(valid.Ok(), "Supported FUObjectItem identity layout was rejected");
		Require(
			valid.SerialOffset == 0x10 && valid.CoherentSamples == 16
				&& valid.PositiveSerialSamples == 16,
			"FUObjectItem identity layout evidence changed");
		Require(
			ValidateEpic64ObjectItemLayoutV1(0x18, 4, 1024, samples).Error
				== ObjectItemLayoutError::UnsupportedObjectOffset,
			"Custom FUObjectItem object offset was guessed");
		Require(
			ValidateEpic64ObjectItemLayoutV1(0x28, 0, 1024, samples).Error
				== ObjectItemLayoutError::UnsupportedItemSize,
			"Unknown FUObjectItem size was guessed");

		for (ObjectItemLayoutSample& sample : samples)
			sample.SerialNumber = 0;
		Require(
			ValidateEpic64ObjectItemLayoutV1(0x18, 0, 1024, samples).Error
				== ObjectItemLayoutError::NoPositiveSerialWitness,
			"Zero-only serial candidate was accepted");
		for (ObjectItemLayoutSample& sample : samples)
			sample.InternalIndex = sample.SlotIndex + 1;
		Require(
			ValidateEpic64ObjectItemLayoutV1(0x18, 0, 1024, samples).Error
				== ObjectItemLayoutError::InsufficientCoherentSamples,
			"Slot/InternalIndex mismatch was accepted");
	}

	void TestSafeMemory()
	{
		using namespace UExplorer::Runtime;

		std::uintptr_t endExclusive = 0;
		Require(!CheckedAddressRange(0, 1, endExclusive), "Null memory range was accepted");
		Require(
			!CheckedAddressRange((std::numeric_limits<std::uintptr_t>::max)() - 1, 4, endExclusive),
			"Overflowing memory range was accepted");

		SYSTEM_INFO systemInfo{};
		GetSystemInfo(&systemInfo);
		void* allocation = VirtualAlloc(
			nullptr,
			systemInfo.dwPageSize,
			MEM_RESERVE | MEM_COMMIT,
			PAGE_READWRITE);
		Require(allocation != nullptr, "SafeMemory fixture allocation failed");
		struct AllocationGuard
		{
			void* Address;
			~AllocationGuard() { if (Address) VirtualFree(Address, 0, MEM_RELEASE); }
		} allocationGuard{allocation};

		const auto address = reinterpret_cast<std::uintptr_t>(allocation);
		const std::uint32_t initial = 0x11223344;
		Require(WriteValue(address, initial).Ok(), "SafeMemory initial write failed");
		std::uint32_t readBack = 0;
		Require(ReadValue(address, readBack).Ok() && readBack == initial, "SafeMemory read changed bytes");

		DWORD oldProtection = 0;
		Require(
			VirtualProtect(allocation, systemInfo.dwPageSize, PAGE_READONLY, &oldProtection) != FALSE,
			"SafeMemory read-only fixture setup failed");
		const std::uint32_t replacement = 0x55667788;
		Require(WriteValue(address, replacement).Ok(), "SafeMemory could not write a read-only data page");
		MEMORY_BASIC_INFORMATION information{};
		Require(
			VirtualQuery(allocation, &information, sizeof(information)) != 0
				&& (information.Protect & 0xFFu) == PAGE_READONLY,
			"SafeMemory did not restore the original page protection");
		Require(ReadValue(address, readBack).Ok() && readBack == replacement, "SafeMemory write was not observable");
		Require(
			WriteValue(address, initial, {.AllowProtectionChange = false}).Error == MemoryError::AccessDenied,
			"SafeMemory ignored a no-protection-change write policy");

		DWORD ignoredProtection = 0;
		Require(
			VirtualProtect(allocation, systemInfo.dwPageSize, PAGE_EXECUTE_READ, &ignoredProtection) != FALSE,
			"SafeMemory executable-page fixture setup failed");
		Require(
			WriteValue(address, initial).Error == MemoryError::ExecutableWriteDenied,
			"SafeMemory allowed an ordinary write to executable memory");
		Require(
			WriteValue(address, initial, {.AllowExecutableWrite = true}).Error
				== MemoryError::InstructionCacheFlushRequired,
			"SafeMemory allowed code writes without an instruction-cache flush");
		Require(
			WriteValue(address, initial, {
				.AllowExecutableWrite = true,
				.FlushInstructionCache = true
			}).Ok(),
			"SafeMemory rejected an explicit code write with cache flushing");

		Require(
			VirtualProtect(allocation, systemInfo.dwPageSize, PAGE_NOACCESS, &ignoredProtection) != FALSE,
			"SafeMemory no-access fixture setup failed");
		Require(
			ReadValue(address, readBack).Error == MemoryError::AccessDenied,
			"SafeMemory attempted to read a no-access page");
		Require(
			VirtualProtect(allocation, systemInfo.dwPageSize, PAGE_READWRITE, &ignoredProtection) != FALSE,
			"SafeMemory fixture protection cleanup failed");

		int first = 1;
		int second = 2;
		void* slot = &first;
		void* observed = nullptr;
		Require(
			CompareExchangePointer(&slot, &first, &second, &observed).Ok()
				&& observed == &first && slot == &second,
			"SafeMemory pointer patch failed");
		Require(
			CompareExchangePointer(&slot, &first, &first).Error == MemoryError::ValueMismatch
				&& slot == &second,
			"SafeMemory pointer patch ignored an expected-value mismatch");
		Require(
			CompareExchangePointer(&slot, &second, &first).Ok() && slot == &first,
			"SafeMemory pointer restore failed");
	}

	void TestQueueOwnershipAndBackpressure()
	{
		using UExplorer::Runtime::BoundedQueue;
		using UExplorer::Runtime::EnqueueResult;
		using UExplorer::Runtime::OverflowPolicy;

		BoundedQueue<std::unique_ptr<int>> tasks(2, OverflowPolicy::RejectNewest);
		Require(tasks.Enqueue(std::make_unique<int>(1)) == EnqueueResult::Accepted, "First task was rejected");
		Require(tasks.Enqueue(std::make_unique<int>(2)) == EnqueueResult::Accepted, "Second task was rejected");
		Require(tasks.Enqueue(std::make_unique<int>(3)) == EnqueueResult::Full, "Full task queue did not reject newest");
		auto first = tasks.TryPop();
		Require(first.has_value() && **first == 1, "Task ownership was not preserved");

		BoundedQueue<int> events(2, OverflowPolicy::DropOldest);
		Require(events.Enqueue(1) == EnqueueResult::Accepted, "First event was rejected");
		Require(events.Enqueue(2) == EnqueueResult::Accepted, "Second event was rejected");
		Require(events.Enqueue(3) == EnqueueResult::DroppedOldest, "Event ring did not report a drop");
		Require(events.DroppedCount() == 1, "Event drop counter is incorrect");
		Require(events.TryPop().value_or(0) == 2, "Event ring did not drop the oldest item");
		Require(events.TryPop().value_or(0) == 3, "Event ring order changed after a drop");
	}

	void TestQueueShutdownWakesWaiters()
	{
		using UExplorer::Runtime::BoundedQueue;
		using UExplorer::Runtime::OverflowPolicy;

		for (int iteration = 0; iteration < 100; ++iteration)
		{
			BoundedQueue<int> queue(1, OverflowPolicy::RejectNewest);
			std::promise<void> waiting;
			auto waitingSignal = waiting.get_future();
			auto consumer = std::async(std::launch::async, [&queue, &waiting] {
				waiting.set_value();
				return queue.WaitPop();
			});
			waitingSignal.wait();
			queue.Close();
			Require(consumer.wait_for(std::chrono::seconds(1)) == std::future_status::ready, "Queue close did not wake a waiter");
			Require(!consumer.get().has_value(), "Closed empty queue returned a value");
			Require(queue.IsClosed(), "Queue did not retain its closed state");
		}
	}

	void TestGameThreadTaskOwnershipAndTimeouts()
	{
		using UExplorer::GameThread::DisableAndDrain;
		using UExplorer::GameThread::Enable;
		using UExplorer::GameThread::HasPending;
		using UExplorer::GameThread::ProcessQueue;
		using UExplorer::GameThread::Submit;
		using UExplorer::GameThread::SubmitResult;

		g_ProcessEventCalls.store(0);
		g_BlockProcessEvent.store(false);
		g_ProcessEventEntered.store(false);
		Require(Enable(&FakeProcessEvent), "Game-thread executor did not enable");

		std::vector<std::uint8_t> params{ 1, 2, 3 };
		auto completed = std::async(std::launch::async, [&params] {
			return Submit(reinterpret_cast<void*>(1), reinterpret_cast<void*>(2), params, 1000);
		});
		WaitUntil(HasPending, "Owned game-thread task was not queued");
		ProcessQueue();
		Require(completed.get() == SubmitResult::Completed, "Owned game-thread task did not complete");
		Require(params[0] == 42, "Owned parameter output was not copied back");
		const auto diagnostics = UExplorer::GameThread::GetDiagnostics();
		Require(diagnostics.PumpObserved, "Game-thread pump tick was not recorded");
		Require(diagnostics.PumpThreadStable, "Game-thread pump thread identity was unstable");
		Require(diagnostics.PumpThreadId == GetCurrentThreadId(), "Game-thread pump thread ID is incorrect");
		Require(DisableAndDrain(), "Completed game-thread executor did not drain");

		Require(Enable(&FakeProcessEvent), "Game-thread executor did not re-enable");
		std::vector<std::uint8_t> queuedParams{ 7 };
		auto queuedTimeout = std::async(std::launch::async, [&queuedParams] {
			return Submit(reinterpret_cast<void*>(1), reinterpret_cast<void*>(2), queuedParams, 50);
		});
		WaitUntil(HasPending, "Timeout test task was not queued");
		Require(
			queuedTimeout.get() == SubmitResult::TimedOutBeforeStart,
			"Queued timeout did not cancel before execution");
		ProcessQueue();
		Require(g_ProcessEventCalls.load() == 1, "Cancelled task executed after timeout");
		Require(DisableAndDrain(), "Cancelled game-thread executor did not drain");

		Require(Enable(&FakeProcessEvent), "Game-thread executor did not enable for running timeout");
		g_BlockProcessEvent.store(true);
		g_ProcessEventEntered.store(false);
		std::vector<std::uint8_t> runningParams{ 9 };
		auto runningTimeout = std::async(std::launch::async, [&runningParams] {
			return Submit(reinterpret_cast<void*>(1), reinterpret_cast<void*>(2), runningParams, 100);
		});
		WaitUntil(HasPending, "Running timeout task was not queued");
		auto processor = std::async(std::launch::async, [] { ProcessQueue(); });
		WaitUntil(
			[] { return g_ProcessEventEntered.load(); },
			"Running timeout task never entered ProcessEvent");
		Require(
			runningTimeout.get() == SubmitResult::TimedOutWhileRunning,
			"Running timeout was not distinguished from queued timeout");
		g_BlockProcessEvent.store(false);
		processor.get();
		Require(DisableAndDrain(), "Running timeout task did not drain after completion");

		Require(Enable(&FakeProcessEvent), "Game-thread executor did not enable for cancellation");
		std::vector<std::uint8_t> cancelledParams{ 3 };
		auto cancelled = std::async(std::launch::async, [&cancelledParams] {
			return Submit(reinterpret_cast<void*>(1), reinterpret_cast<void*>(2), cancelledParams, 1000);
		});
		WaitUntil(HasPending, "Shutdown cancellation task was not queued");
		Require(DisableAndDrain(), "Disable did not drain a queued task");
		Require(cancelled.get() == SubmitResult::Cancelled, "Disable did not cancel the queued submitter");

		Require(Enable(&FaultingProcessEvent), "Game-thread executor did not enable for SEH test");
		std::vector<std::uint8_t> faultParams{ 1 };
		auto faulted = std::async(std::launch::async, [&faultParams] {
			return Submit(reinterpret_cast<void*>(1), reinterpret_cast<void*>(2), faultParams, 1000);
		});
		WaitUntil(HasPending, "SEH test task was not queued");
		ProcessQueue();
		Require(faulted.get() == SubmitResult::ExecutionFailed, "ProcessEvent SEH did not become a terminal failure");
		Require(DisableAndDrain(), "SEH failure left the game-thread executor processing");
	}

	void TestGenericGameThreadWorkAndCancellation()
	{
		using namespace UExplorer::Runtime;

		GameThreadExecutor& executor = GetGameThreadExecutor();
		Require(executor.Enable(&FakeProcessEvent), "Generic game-thread executor did not enable");
		auto work = std::make_shared<OwnedProbeWork>(21);
		auto submitted = std::async(std::launch::async, [&executor, work] {
			return executor.SubmitOwned(work, 1000);
		});
		WaitUntil([&executor] { return executor.HasPending(); }, "Generic owned work was not queued");
		GetPostRenderPumpBackend().Tick();
		Require(
			submitted.get() == GameThreadSubmitResult::Completed,
			"Generic owned game-thread work did not complete");
		Require(work->Output == 42, "Generic work did not retain owned input/result state");
		Require(
			work->ExecutionThreadId == GetCurrentThreadId() && executor.IsCurrentPumpThread(),
			"Generic work did not execute on the observed PostRender pump thread");
		auto reentrantWork = std::make_shared<OwnedProbeWork>(3);
		Require(
			executor.SubmitOwned(reentrantWork, 100)
				== GameThreadSubmitResult::PumpThreadWaitDenied,
			"A synchronous submit was allowed to deadlock the PostRender pump thread");
		Require(!executor.HasPending(), "Rejected pump-thread submit was still queued");
		GameThreadTicket unboundedTicket;
		Require(
			executor.Enqueue(
				std::make_shared<OwnedProbeWork>(5),
				std::chrono::steady_clock::now()
					+ std::chrono::milliseconds(GameThreadExecutor::kMaxTimeoutMs + 1),
				unboundedTicket) == GameThreadQueueResult::Invalid,
			"Executor accepted a task beyond the protocol deadline limit");

		GameThreadTicket cancelledTicket;
		auto cancelledWork = std::make_shared<OwnedProbeWork>(7);
		Require(
			executor.Enqueue(
				cancelledWork,
				std::chrono::steady_clock::now() + std::chrono::seconds(1),
				cancelledTicket) == GameThreadQueueResult::Accepted,
			"Cancellable game-thread work was not accepted");
		Require(
			executor.Cancel(cancelledTicket) == GameThreadCancelResult::Cancelled,
			"Queued game-thread work was not explicitly cancelled");
		Require(
			executor.Wait(cancelledTicket) == GameThreadSubmitResult::Cancelled,
			"Cancelled game-thread work did not reach a terminal state");
		GetPostRenderPumpBackend().Tick();
		Require(cancelledWork->Output == 0, "Cancelled generic work executed after cancellation");

		auto throwingWork = std::make_shared<OwnedProbeWork>(1, true);
		auto throwing = std::async(std::launch::async, [&executor, throwingWork] {
			return executor.SubmitOwned(throwingWork, 1000);
		});
		WaitUntil([&executor] { return executor.HasPending(); }, "Throwing generic work was not queued");
		GetPostRenderPumpBackend().Tick();
		Require(
			throwing.get() == GameThreadSubmitResult::ExecutionFailed,
			"Generic C++ exception did not become a terminal execution failure");

		auto mismatchWork = std::make_shared<OwnedProbeWork>(9);
		auto mismatched = std::async(std::launch::async, [&executor, mismatchWork] {
			return executor.SubmitOwned(mismatchWork, 100);
		});
		WaitUntil([&executor] { return executor.HasPending(); }, "Thread-mismatch work was not queued");
		auto wrongThreadTick = std::async(std::launch::async, [] {
			GetPostRenderPumpBackend().Tick();
		});
		wrongThreadTick.get();
		Require(
			mismatched.get() == GameThreadSubmitResult::TimedOutBeforeStart,
			"Cross-thread PostRender executed queued work instead of disabling the pump");
		Require(mismatchWork->Output == 0, "Cross-thread PostRender executed owned work");
		Require(
			!executor.GetDiagnostics().PumpThreadStable,
			"Cross-thread PostRender did not persist a pump mismatch diagnostic");
		Require(executor.DisableAndDrain(), "Generic game-thread executor did not drain");
	}

	void TestGameThreadMpscCapacity()
	{
		using UExplorer::GameThread::DisableAndDrain;
		using UExplorer::GameThread::Enable;
		using UExplorer::GameThread::QueueDepth;
		using UExplorer::GameThread::Submit;
		using UExplorer::GameThread::SubmitResult;

		Require(Enable(&FakeProcessEvent), "Game-thread executor did not enable for MPSC capacity test");
		std::vector<std::vector<std::uint8_t>> params(UExplorer::GameThread::kQueueCapacity, { 1 });
		std::vector<SubmitResult> results(params.size(), SubmitResult::ExecutionFailed);
		std::vector<std::thread> submitters;
		submitters.reserve(params.size());
		for (std::size_t index = 0; index < params.size(); ++index)
		{
			submitters.emplace_back([&params, &results, index] {
				results[index] = Submit(
					reinterpret_cast<void*>(1),
					reinterpret_cast<void*>(2),
					params[index],
					10000);
			});
		}
		WaitUntil(
			[] { return QueueDepth() == UExplorer::GameThread::kQueueCapacity; },
			"MPSC producers did not fill the bounded queue");

		std::vector<std::uint8_t> overflowParams{ 1 };
		Require(
			Submit(reinterpret_cast<void*>(1), reinterpret_cast<void*>(2), overflowParams, 20)
				== SubmitResult::QueueBusy,
			"Full game-thread queue did not reject at its deadline");
		Require(DisableAndDrain(), "Bounded MPSC queue did not drain during shutdown");
		for (auto& submitter : submitters)
			submitter.join();
		for (const auto result : results)
			Require(result == SubmitResult::Cancelled, "Shutdown did not cancel an MPSC producer task");
	}

	SOCKET ConnectLoopback(const std::uint16_t port)
	{
		SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		Require(client != INVALID_SOCKET, "HTTP lifecycle client socket creation failed");
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons(port);
		if (connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
		{
			closesocket(client);
			throw std::runtime_error("HTTP lifecycle client failed to connect");
		}
		return client;
	}

	void TestHttpServerLifecycle()
	{
		UExplorer::HttpServer server(0, "test-token");
		server.Get("/api/v1/status/health", [](const UExplorer::HttpRequest&) {
			return UExplorer::HttpResponse{ 200, "application/json", R"({"ok":true})" };
		});
		Require(server.Start(), "HTTP lifecycle server did not start on an explicitly dynamic port");
		const std::uint16_t port = server.GetPort();
		Require(port != 0, "HTTP lifecycle server did not publish its bound port");

		UExplorer::HttpServer conflictingServer(port, "test-token");
		Require(!conflictingServer.Start(), "HTTP server silently fell back from an occupied exact port");

		static constexpr char request[] =
			"GET /api/v1/status/health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
		for (int iteration = 0; iteration < 1000; ++iteration)
		{
			SOCKET client = ConnectLoopback(port);
			Require(
				send(client, request, static_cast<int>(sizeof(request) - 1), 0)
					== static_cast<int>(sizeof(request) - 1),
				"HTTP lifecycle request send was incomplete");
			std::string response;
			char buffer[512];
			for (;;)
			{
				const int received = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
				if (received <= 0)
					break;
				response.append(buffer, static_cast<std::size_t>(received));
			}
			closesocket(client);
			Require(response.find("HTTP/1.1 200 OK") == 0, "HTTP lifecycle request did not complete");
		}

		SOCKET slowClient = ConnectLoopback(port);
		static constexpr char partialRequest = 'G';
		Require(send(slowClient, &partialRequest, 1, 0) == 1, "Slow client setup failed");
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		const auto stopStarted = std::chrono::steady_clock::now();
		Require(server.Stop(), "HTTP server retained a worker after Stop");
		const auto stopElapsed = std::chrono::steady_clock::now() - stopStarted;
		closesocket(slowClient);
		Require(stopElapsed < std::chrono::seconds(3), "HTTP server Stop did not interrupt a slow client");
	}
}

int main(const int argc, char** argv)
{
	try
	{
		if (argc != 2)
			throw std::runtime_error("Usage: CoreHarness.exe <protocol-fixture-directory>");
		const std::filesystem::path fixtureDirectory(argv[1]);
		TestGoldenHello(fixtureDirectory);
		TestMultipleFrames();
		TestTerminalErrors();
		TestUsmapContainer(fixtureDirectory);
		TestEngineContextAndCapabilities();
		TestCoreRuntimeStateAndShutdown();
		TestStableObjectAndFunctionHandles();
		TestFUObjectItemIdentityLayout();
		TestHookOwnershipAndCallbackDrain();
		TestSafeMemory();
		TestQueueOwnershipAndBackpressure();
		TestQueueShutdownWakesWaiters();
		TestGameThreadTaskOwnershipAndTimeouts();
		TestGenericGameThreadWorkAndCancellation();
		TestGameThreadMpscCapacity();
		TestHttpServerLifecycle();
		std::cout << "Core harness passed: framing, runtime/capabilities, stable handles/FUObjectItem layout, Hook RAII/drain, SafeMemory, USMAP consumer, bounded queues, cancellable owned game-thread work, SEH, HTTP lifecycle, and shutdown.\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Core harness failed: " << error.what() << '\n';
		return 1;
	}
}
