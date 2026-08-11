#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include "IPC/Protocol.h"
#include "IPC/NamedPipeRpcServer.h"
#include "OffsetFinder/OffsetDiscovery.h"
#include "Platform/Public/BytePattern.h"
#include "Platform/Public/PeImage.h"
#include "Runtime/BoundedQueue.h"
#include "Runtime/CallbackBarrier.h"
#include "Runtime/CoreCapabilities.h"
#include "Runtime/CoreRuntime.h"
#include "Runtime/CoreSession.h"
#include "Runtime/EngineFacade.h"
#include "Runtime/EngineNameCodec.h"
#include "Runtime/EngineSnapshot.h"
#include "Runtime/EngineSnapshotCapture.h"
#include "Runtime/EngineVersionProbe.h"
#include "Runtime/FUObjectItemLayout.h"
#include "Runtime/GameThreadExecutor.h"
#include "Runtime/GameThreadFrameScheduler.h"
#include "Runtime/ObjectHandle.h"
#include "Runtime/ObjectIdentityContext.h"
#include "Runtime/ObjectArraySnapshotSource.h"
#include "Runtime/ObjectSnapshotReflectionCandidateSource.h"
#include "Runtime/ObjectSnapshotTypeCandidateSource.h"
#include "Runtime/ParamFrame.h"
#include "Runtime/PropertyCodec.h"
#include "Runtime/ReflectionLayout.h"
#include "Runtime/ReflectionLayoutCapture.h"
#include "Runtime/TypeSnapshotCapture.h"
#include "Runtime/WorldSnapshot.h"
#include "Runtime/SafeMemory.h"
#include "Runtime/ShutdownCoordinator.h"
#include "Runtime/VTableHook.h"
#include "Services/CoreCommandService.h"
#include "Services/TypeCommandService.h"
#include "Services/WorldCommandService.h"
#include "API/GameThreadQueue.h"
#include "Generator/Public/Generators/UsmapContainer.h"
#include "Server/HttpServer.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sstream>
#include <span>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

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

	template<typename T>
	void StoreFixtureValue(
		std::vector<std::byte>& bytes,
		const std::size_t offset,
		const T& value)
	{
		static_assert(std::is_trivially_copyable_v<T>);
		Require(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset,
			"Fixture write exceeded its backing buffer");
		std::memcpy(bytes.data() + offset, &value, sizeof(T));
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

	void WritePipeBytes(const HANDLE pipe, const std::vector<std::uint8_t>& bytes)
	{
		std::size_t offset = 0;
		while (offset < bytes.size())
		{
			DWORD written = 0;
			const DWORD chunk = static_cast<DWORD>((std::min)(
				bytes.size() - offset,
				UExplorer::IPC::NamedPipeRpcServer::kIoChunkBytes));
			Require(
				WriteFile(pipe, bytes.data() + offset, chunk, &written, nullptr) != FALSE
					&& written > 0,
				"Named-pipe fixture write failed");
			offset += written;
		}
	}

	void ReadPipeBytes(const HANDLE pipe, std::uint8_t* output, const std::size_t size)
	{
		std::size_t offset = 0;
		while (offset < size)
		{
			DWORD read = 0;
			const DWORD chunk = static_cast<DWORD>((std::min)(
				size - offset,
				UExplorer::IPC::NamedPipeRpcServer::kIoChunkBytes));
			if (ReadFile(pipe, output + offset, chunk, &read, nullptr) == FALSE || read == 0)
			{
				throw std::runtime_error(
					"Named-pipe fixture read failed: native=" + std::to_string(GetLastError())
					+ " offset=" + std::to_string(offset)
					+ " expected=" + std::to_string(size));
			}
			offset += read;
		}
	}

	void WriteJsonPipeFrame(
		const HANDLE pipe,
		const UExplorer::IPC::FrameKind kind,
		const std::uint64_t requestId,
		const nlohmann::json& payload)
	{
		const std::string serialized = payload.dump();
		const auto payloadBytes = std::span<const std::uint8_t>(
			reinterpret_cast<const std::uint8_t*>(serialized.data()),
			serialized.size());
		std::vector<std::uint8_t> encoded;
		Require(
			UExplorer::IPC::EncodeFrame(kind, requestId, payloadBytes, encoded)
				== UExplorer::IPC::ProtocolError::None,
			"Named-pipe fixture frame encode failed");
		WritePipeBytes(pipe, encoded);
	}

	UExplorer::IPC::Frame ReadPipeFrame(const HANDLE pipe)
	{
		using namespace UExplorer::IPC;
		std::array<std::uint8_t, HeaderSize> headerBytes{};
		ReadPipeBytes(pipe, headerBytes.data(), headerBytes.size());
		Frame frame;
		Require(
			DecodeHeader(headerBytes, frame.Header) == ProtocolError::None,
			"Named-pipe fixture received an invalid frame header");
		frame.Payload.resize(frame.Header.PayloadLength);
		if (!frame.Payload.empty())
			ReadPipeBytes(pipe, frame.Payload.data(), frame.Payload.size());
		return frame;
	}

	nlohmann::json ParseFrameJson(const UExplorer::IPC::Frame& frame)
	{
		const nlohmann::json payload = nlohmann::json::parse(
			frame.Payload.begin(), frame.Payload.end(), nullptr, false);
		Require(!payload.is_discarded(), "Named-pipe fixture received invalid JSON");
		return payload;
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

	class FrameClientProbe final : public UExplorer::Runtime::IGameThreadFrameClient
	{
	public:
		PumpResult PumpFrame(const std::size_t workBudget) noexcept override
		{
			Calls.fetch_add(1, std::memory_order_acq_rel);
			LastBudget.store(workBudget, std::memory_order_release);
			ExecutionThreadId.store(GetCurrentThreadId(), std::memory_order_release);
			Entered.store(true, std::memory_order_release);
			while (Block.load(std::memory_order_acquire))
				std::this_thread::yield();
			const int sleepMilliseconds = SleepMilliseconds.load(std::memory_order_acquire);
			if (sleepMilliseconds > 0)
				std::this_thread::sleep_for(std::chrono::milliseconds(sleepMilliseconds));
			const std::size_t consumed = OverReport.load(std::memory_order_acquire)
				? workBudget + 1
				: (ConsumeBudget.load(std::memory_order_acquire) ? workBudget : 0);
			WorkConsumed.fetch_add(consumed, std::memory_order_acq_rel);
			return {
				.WorkConsumed = consumed,
				.MoreWorkPending = MoreWorkPending.load(std::memory_order_acquire)
			};
		}

		std::atomic<int> Calls{0};
		std::atomic<bool> Block{false};
		std::atomic<bool> Entered{false};
		std::atomic<bool> ConsumeBudget{false};
		std::atomic<bool> MoreWorkPending{false};
		std::atomic<bool> OverReport{false};
		std::atomic<int> SleepMilliseconds{0};
		std::atomic<std::size_t> LastBudget{0};
		std::atomic<std::size_t> WorkConsumed{0};
		std::atomic<std::uint32_t> ExecutionThreadId{0};
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
		Require(
			sizeDecoder.Push({}).Error == ProtocolError::DecoderFailed,
			"Terminal framing error did not poison the decoder");

		FrameDecoder negotiatedDecoder;
		Require(
			negotiatedDecoder.SetPayloadLimit(32) == ProtocolError::None,
			"Valid negotiated decoder limit was rejected");
		auto negotiatedOversized = frame;
		Detail::WriteU32(negotiatedOversized.data() + 12, 33);
		Require(
			negotiatedDecoder.Push(negotiatedOversized).Error == ProtocolError::PayloadTooLarge
				&& negotiatedDecoder.BufferedBytes() == 0,
			"Negotiated decoder limit was not enforced before body buffering");

		std::vector<std::uint8_t> coalesced;
		coalesced.reserve(frame.size() * 1024 + HeaderSize - 1);
		for (std::size_t index = 0; index < 1024; ++index)
			coalesced.insert(coalesced.end(), frame.begin(), frame.end());
		coalesced.insert(coalesced.end(), frame.begin(), frame.begin() + HeaderSize - 1);
		FrameDecoder streamingDecoder;
		const DecodeBatch streamed = streamingDecoder.Push(coalesced);
		Require(
			streamed.Error == ProtocolError::None
				&& streamed.Frames.size() == 1024
				&& streamingDecoder.BufferedBytes() == HeaderSize - 1,
			"Coalesced input was duplicated into an unbounded decoder buffer");
	}

	void TestFrameDecoderFuzzMatrix()
	{
		constexpr std::array<FrameKind, 9> kinds{
			FrameKind::Hello,
			FrameKind::Welcome,
			FrameKind::Request,
			FrameKind::Response,
			FrameKind::Event,
			FrameKind::Cancel,
			FrameKind::Ping,
			FrameKind::Pong,
			FrameKind::Shutdown
		};
		std::uint64_t randomState = 0xD1B54A32D192ED03ULL;
		auto nextRandom = [&randomState] {
			randomState ^= randomState >> 12U;
			randomState ^= randomState << 25U;
			randomState ^= randomState >> 27U;
			return randomState * 0x2545F4914F6CDD1DULL;
		};

		std::vector<Frame> expected;
		std::vector<std::uint8_t> stream;
		for (std::uint64_t index = 0; index < 256; ++index)
		{
			std::vector<std::uint8_t> payload(static_cast<std::size_t>(nextRandom() % 513));
			for (std::uint8_t& byte : payload)
				byte = static_cast<std::uint8_t>(nextRandom());
			const FrameKind kind = kinds[static_cast<std::size_t>(nextRandom() % kinds.size())];
			std::vector<std::uint8_t> encoded;
			Require(
				EncodeFrame(kind, index + 1, payload, encoded) == ProtocolError::None,
				"Fuzz matrix could not encode a bounded source frame");
			stream.insert(stream.end(), encoded.begin(), encoded.end());
			expected.push_back(Frame{
				.Header = {
					.Major = ProtocolMajor,
					.Minor = ProtocolMinor,
					.Kind = kind,
					.Flags = 0,
					.PayloadLength = static_cast<std::uint32_t>(payload.size()),
					.RequestId = index + 1
				},
				.Payload = std::move(payload)
			});
		}

		constexpr std::array<std::size_t, 8> chunkSizes{1, 2, 3, 7, 23, 64, 257, 4096};
		for (const std::size_t chunkSize : chunkSizes)
		{
			FrameDecoder decoder;
			std::vector<Frame> actual;
			for (std::size_t offset = 0; offset < stream.size();)
			{
				const std::size_t take = (std::min)(chunkSize, stream.size() - offset);
				const DecodeBatch batch = decoder.Push(
					std::span<const std::uint8_t>(stream).subspan(offset, take));
				Require(batch.Error == ProtocolError::None, "Valid fuzz stream entered a terminal state");
				actual.insert(actual.end(), batch.Frames.begin(), batch.Frames.end());
				offset += take;
			}
			Require(actual.size() == expected.size(), "Chunk matrix lost or duplicated a frame");
			for (std::size_t index = 0; index < expected.size(); ++index)
			{
				Require(
					actual[index].Header.Kind == expected[index].Header.Kind
						&& actual[index].Header.RequestId == expected[index].Header.RequestId
						&& actual[index].Payload == expected[index].Payload,
					"Chunk matrix changed frame identity or payload bytes");
			}
			Require(decoder.BufferedBytes() == 0, "Chunk matrix retained a complete stream tail");
		}

		std::vector<std::uint8_t> truncationFrame;
		const std::vector<std::uint8_t> truncationPayload(257, 0xA5);
		Require(
			EncodeFrame(FrameKind::Event, 0, truncationPayload, truncationFrame)
				== ProtocolError::None,
			"Disconnect matrix source frame did not encode");
		for (std::size_t prefix = 0; prefix < truncationFrame.size(); ++prefix)
		{
			FrameDecoder decoder;
			const DecodeBatch batch = decoder.Push(
				std::span<const std::uint8_t>(truncationFrame).first(prefix));
			Require(
				batch.Error == ProtocolError::None
					&& batch.Frames.empty()
					&& decoder.BufferedBytes() == prefix,
				"Truncated frame was misparsed before disconnect");
		}

		for (std::size_t iteration = 0; iteration < 4096; ++iteration)
		{
			std::vector<std::uint8_t> mutated = truncationFrame;
			const std::size_t flips = 1 + static_cast<std::size_t>(nextRandom() % 4);
			for (std::size_t flip = 0; flip < flips; ++flip)
			{
				const std::size_t offset = static_cast<std::size_t>(nextRandom() % mutated.size());
				mutated[offset] ^= static_cast<std::uint8_t>(1U << (nextRandom() % 8));
			}

			FrameDecoder decoder;
			std::size_t offset = 0;
			ProtocolError terminal = ProtocolError::None;
			while (offset < mutated.size() && terminal == ProtocolError::None)
			{
				const std::size_t chunk = 1 + static_cast<std::size_t>(nextRandom() % 31);
				const std::size_t take = (std::min)(chunk, mutated.size() - offset);
				terminal = decoder.Push(
					std::span<const std::uint8_t>(mutated).subspan(offset, take)).Error;
				offset += take;
			}
			Require(
				decoder.BufferedBytes() <= mutated.size(),
				"Mutated frame caused decoder amplification");
			if (terminal != ProtocolError::None)
			{
				Require(
					decoder.Push({}).Error == ProtocolError::DecoderFailed,
					"Mutated terminal frame did not poison the decoder");
			}
		}
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
		const std::uint64_t generation = 1,
		const bool includeFunctionIdentity = true,
		const bool includeNameProfile = true,
		const UExplorer::Runtime::EngineNameProfile* nameProfileOverride = nullptr,
		const std::uint32_t processId = 4242,
		const bool usesFProperty = true)
	{
		UExplorer::Runtime::EngineContextBuilder builder(generation);
		builder.SetIdentity(0x140000000, 0x140100000, processId, 100, "FixtureGame", "5.4");
		builder.SetProfile({
			.UsesFProperty = usesFProperty,
			.UsesLargeWorldCoordinates = true
		});
		if (nameProfileOverride)
		{
			builder.SetNameProfile(*nameProfileOverride);
		}
		else if (includeNameProfile)
		{
			builder.SetNameProfile({
				.Storage = UExplorer::Runtime::EngineNameStorageKind::NamePool,
				.StorageAddress = 0x140200000,
				.FNameSize = 8,
				.ComparisonIndexOffset = 0,
				.NumberOffset = 4,
				.BlockOffsetBits = 14,
				.EntryStride = 2,
				.ChunksStart = 16,
				.MaxChunkIndexOffset = 0,
				.ByteCursorOffset = 4,
				.EntryStringOffset = 2,
				.EntryHeaderOffset = 0,
				.EntryLengthShift = 6,
				.Validated = true,
				.Source = "harness",
				.Checks = {"fixture"}
			});
		}
		builder.AddOffset(ValidatedOffset("gobjects", 0x100000, true));
		builder.AddOffset(ValidatedOffset("gworld", 0x200000));
		builder.AddOffset(ValidatedOffset("process_event.index", 0x4C, true));
		builder.AddOffset(ValidatedOffset("process_event.offset", 0x300000, true));
		builder.AddOffset(ValidatedOffset("fuobjectitem.serial_number", 0x10));
		builder.AddOffset(ValidatedOffset("uobject.index", 0x0C));
		builder.AddOffset(ValidatedOffset("uobject.class", 0x10));
		builder.AddOffset(ValidatedOffset("uobject.name", 0x18));
		builder.AddOffset(ValidatedOffset("uobject.outer", 0x20));
		builder.AddOffset(ValidatedOffset("fname.size", 0x08));
		builder.AddOffset(ValidatedOffset("fname.comparison_index", 0x00));
		builder.AddOffset(ValidatedOffset("fname.number", 0x04));
		builder.AddOffset(ValidatedOffset("uclass.cast_flags", 0x38));
		builder.AddOffset(ValidatedOffset("uclass.default_object", 0xE0));
		builder.AddOffset(ValidatedOffset("ufunction.function_flags", 0xB0));
		if (includeFunctionIdentity)
			builder.AddOffset(ValidatedOffset("ufunction.exec_function", 0xD8));
		builder.AddOffset(ValidatedOffset("ustruct.size", 0x58));
		return builder.Build();
	}

	void TestCoreSessionIdentity()
	{
		using namespace UExplorer::Runtime;

		std::string first;
		std::string second;
		Require(TryGenerateCoreSessionId(first), "Secure Core session generation failed");
		Require(TryGenerateCoreSessionId(second), "Second secure Core session generation failed");
		Require(first.size() == 37 && first.starts_with("core-"), "Core session format changed");
		Require(first != second, "Core session generator repeated a fixture identity");
		for (const char value : first.substr(5))
		{
			Require(
				(value >= '0' && value <= '9') || (value >= 'A' && value <= 'F'),
				"Core session contains a non-hex random byte");
		}

		CoreRuntime invalidRuntime;
		Require(!invalidRuntime.BeginInitialize(""), "CoreRuntime accepted an empty session identity");
		Require(
			!invalidRuntime.BeginInitialize("invalid session"),
			"CoreRuntime accepted a session identity outside the protocol alphabet");
	}

	void TestEngineContextAndCapabilities()
	{
		using namespace UExplorer::Runtime;

		const auto context = MakeEngineContext();
		Require(context->Generation() == 1, "Engine context generation changed");
		Require(context->HasValidatedOffset("gobjects"), "Validated offset was not published");
		Require(context->Profile().UsesFProperty, "Immutable engine profile was not published");
		Require(context->NameProfile().Validated, "Immutable name profile was not published");
		const ObjectIdentityContext identityContext = CaptureObjectIdentityContext(*context);
		Require(
			identityContext.CanIssueObjectHandles()
				&& identityContext.CanIssueFunctionHandles(),
			"Immutable context did not configure stable object/function identities");
		const ObjectIdentityContext objectOnlyIdentity =
			CaptureObjectIdentityContext(*MakeEngineContext(2, false));
		Require(
			objectOnlyIdentity.CanIssueObjectHandles()
				&& !objectOnlyIdentity.CanIssueFunctionHandles(),
			"Missing function metadata did not disable only function handles");

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
		probes.FunctionHandleValidationEnabled = true;
		const auto withoutCallService = BuildCoreCapabilities(*context, probes);
		Require(
			withoutCallService->IsAvailable("engine.names")
				&& withoutCallService->IsAvailable("objects.handles")
				&& !withoutCallService->IsAvailable("objects.snapshot")
				&& !withoutCallService->IsAvailable("call.invoke"),
			"Handle capability falsely enabled an unregistered function-call command");
		probes.ObjectSnapshotPublished = true;
		probes.FunctionCallServiceEnabled = true;
		const auto withoutPipe = BuildCoreCapabilities(*context, probes);
		Require(!withoutPipe->IsAvailable("transport.named_pipe"), "Missing pipe listener was advertised");
		Require(withoutPipe->IsAvailable("objects.snapshot"), "Published immutable snapshot was unavailable");
		Require(
			!withoutPipe->IsAvailable("call.invoke"),
			"Function invocation ignored the missing type snapshot and property codec");
		const CapabilityStatus* reflection = withoutPipe->Find("engine.reflection");
		Require(
			reflection
				&& !reflection->Available
				&& reflection->ReasonCode == "REFLECTION_RUNTIME_NOT_PUBLISHED"
				&& !withoutPipe->IsAvailable("engine.property_codec")
				&& !withoutPipe->IsAvailable("objects.properties")
				&& !withoutPipe->IsAvailable("types.inspect"),
			"Unvalidated optional reflection metadata leaked into a domain capability");
		RuntimeProbes falselyClaimedReflection = probes;
		falselyClaimedReflection.Reflection =
			std::make_shared<const ReflectionRuntimeSnapshot>();
		const auto withoutCompleteReflection = BuildCoreCapabilities(
			*context,
			falselyClaimedReflection);
		const CapabilityStatus* incompleteReflection =
			withoutCompleteReflection->Find("engine.reflection");
		Require(
			incompleteReflection
				&& !incompleteReflection->Available
				&& incompleteReflection->ReasonCode == "REFLECTION_RUNTIME_INVALID"
				&& !withoutCompleteReflection->IsAvailable("engine.property_codec"),
			"An incomplete reflection snapshot bypassed the immutable layout requirement");
		RuntimeProbes missingFunctionHandles = probes;
		missingFunctionHandles.FunctionHandleValidationEnabled = false;
		const auto withoutFunctionHandles = BuildCoreCapabilities(*context, missingFunctionHandles);
		Require(
			withoutFunctionHandles->IsAvailable("objects.handles")
				&& !withoutFunctionHandles->IsAvailable("functions.handles")
				&& !withoutFunctionHandles->IsAvailable("call.invoke"),
			"Function call capability ignored the function-handle dependency");
		RuntimeProbes missingIdentitySource = probes;
		missingIdentitySource.ObjectIdentitySourceEnabled = false;
		const auto withoutIdentitySource = BuildCoreCapabilities(*context, missingIdentitySource);
		Require(
			!withoutIdentitySource->IsAvailable("objects.handles")
				&& !withoutIdentitySource->IsAvailable("call.invoke"),
			"Handle/call capability ignored the production identity source dependency");
		const auto withoutNameProfile = BuildCoreCapabilities(*MakeEngineContext(3, true, false), probes);
		Require(
			!withoutNameProfile->IsAvailable("engine.names")
				&& !withoutNameProfile->IsAvailable("objects.snapshot"),
			"Snapshot capability ignored the immutable name-layout dependency");
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

	void TestEngineNameCodec()
	{
		using namespace UExplorer::Runtime;

		const auto writeBytes = [](auto& buffer, const std::size_t offset, const auto& value) {
			Require(offset <= buffer.size() && sizeof(value) <= buffer.size() - offset,
				"Name codec fixture write exceeded its buffer");
			std::memcpy(buffer.data() + offset, &value, sizeof(value));
		};
		const auto writeString = [](auto& buffer, const std::size_t offset, const void* value, const std::size_t size) {
			Require(offset <= buffer.size() && size <= buffer.size() - offset,
				"Name codec string fixture write exceeded its buffer");
			std::memcpy(buffer.data() + offset, value, size);
		};

		std::array<std::byte, 64> pool{};
		std::array<std::byte, 96> block{};
		writeBytes(pool, 0, std::int32_t{0});
		writeBytes(pool, 4, std::int32_t{64});
		writeBytes(pool, 16, reinterpret_cast<std::uintptr_t>(block.data()));

		writeBytes(block, 2, static_cast<std::uint16_t>(5u << 6));
		constexpr char actor[] = "Actor";
		writeString(block, 4, actor, 5);
		writeBytes(block, 10, static_cast<std::uint16_t>((2u << 6) | 1u));
		const std::array<wchar_t, 2> wideName{L'\u6D4B', L'\u8BD5'};
		writeString(block, 12, wideName.data(), sizeof(wideName));

		writeBytes(block, 16, std::uint16_t{0});
		writeBytes(block, 18, std::int32_t{1});
		writeBytes(block, 22, std::int32_t{3});
		writeBytes(block, 32, std::uint16_t{0});
		writeBytes(block, 34, std::int32_t{22});
		writeBytes(block, 38, std::int32_t{0});
		writeBytes(block, 44, std::uint16_t{0});
		writeBytes(block, 46, std::int32_t{16});
		writeBytes(block, 50, std::int32_t{0});
		writeBytes(block, 56, static_cast<std::uint16_t>(1u << 6));
		const std::uint8_t invalidUtf8 = 0xFF;
		writeBytes(block, 58, invalidUtf8);

		EngineNameProfile poolProfile{
			.Storage = EngineNameStorageKind::NamePool,
			.StorageAddress = reinterpret_cast<std::uintptr_t>(pool.data()),
			.FNameSize = 8,
			.ComparisonIndexOffset = 0,
			.NumberOffset = 4,
			.BlockOffsetBits = 14,
			.EntryStride = 2,
			.ChunksStart = 16,
			.MaxChunkIndexOffset = 0,
			.ByteCursorOffset = 4,
			.EntryStringOffset = 2,
			.EntryHeaderOffset = 0,
			.EntryLengthShift = 6,
			.Validated = true,
			.Source = "name-pool-fixture"
		};
		EngineNameCodec poolCodec(poolProfile);
		Require(poolCodec.IsConfigured(), "Validated NamePool profile was rejected");
		const EngineNameResult numbered = poolCodec.Decode(1, 2);
		Require(numbered.Ok() && numbered.Value == "Actor_1", "NamePool number suffix decoded incorrectly");
		const EngineNameResult wide = poolCodec.Decode(5);
		Require(
			wide.Ok() && wide.Value == "\xE6\xB5\x8B\xE8\xAF\x95",
			"Wide NamePool entry did not produce strict UTF-8");
		const std::array<std::uint32_t, 2> fname{1, 2};
		const EngineNameResult decodedFName = poolCodec.DecodeFName(
			reinterpret_cast<std::uintptr_t>(fname.data()));
		Require(decodedFName.Ok() && decodedFName.Value == "Actor_1", "FName field layout was ignored");
		Require(
			poolCodec.Decode(1000).Error == EngineNameError::IndexOutOfRange,
			"NamePool byte cursor did not bound an index");
		Require(
			poolCodec.Decode(28).Error == EngineNameError::EncodingInvalid,
			"Invalid UTF-8 FName entry was accepted");

		EngineNameProfile outlineProfile = poolProfile;
		outlineProfile.FNameSize = 4;
		outlineProfile.NumberOffset = -1;
		outlineProfile.UsesOutlineNumber = true;
		EngineNameCodec outlineCodec(outlineProfile);
		const EngineNameResult redirected = outlineCodec.Decode(8);
		Require(
			redirected.Ok() && redirected.Value == "Actor_2",
			"Outline-number redirect did not preserve its suffix");
		Require(
			outlineCodec.Decode(16).Error == EngineNameError::RedirectCycle,
			"NamePool redirect cycle was not rejected");
		Require(
			outlineCodec.Decode(1, 1).Error == EngineNameError::NumberInvalid,
			"Outline-number profile accepted an inline FName number");

		std::array<std::byte, 32> legacyStorage{};
		std::vector<std::uintptr_t> legacyChunk(0x4000);
		std::array<std::byte, 64> legacyNarrow{};
		std::array<std::byte, 64> legacyWide{};
		writeBytes(legacyNarrow, 0, std::uint32_t{0});
		constexpr char legacyText[] = "Legacy";
		writeString(legacyNarrow, 4, legacyText, sizeof(legacyText));
		writeBytes(legacyWide, 0, std::uint32_t{3});
		const std::array<wchar_t, 3> legacyWideText{L'W', L'i', L'\0'};
		writeString(legacyWide, 4, legacyWideText.data(), sizeof(legacyWideText));
		legacyChunk[0] = reinterpret_cast<std::uintptr_t>(legacyNarrow.data());
		legacyChunk[1] = reinterpret_cast<std::uintptr_t>(legacyWide.data());
		writeBytes(legacyStorage, 0, reinterpret_cast<std::uintptr_t>(legacyChunk.data()));
		writeBytes(legacyStorage, 8, std::int32_t{2});
		writeBytes(legacyStorage, 12, std::int32_t{1});

		EngineNameProfile legacyProfile{
			.Storage = EngineNameStorageKind::ChunkedArray,
			.StorageAddress = reinterpret_cast<std::uintptr_t>(legacyStorage.data()),
			.FNameSize = 8,
			.ComparisonIndexOffset = 0,
			.NumberOffset = 4,
			.ChunksStart = 0,
			.MaxChunkIndexOffset = 12,
			.NumElementsOffset = 8,
			.EntryStringOffset = 4,
			.EntryIndexOffset = 0,
			.Validated = true,
			.Source = "legacy-name-array-fixture"
		};
		EngineNameCodec legacyCodec(legacyProfile);
		Require(legacyCodec.IsConfigured(), "Validated chunked name-array profile was rejected");
		const EngineNameResult legacyNumbered = legacyCodec.Decode(0, 4);
		Require(
			legacyNumbered.Ok() && legacyNumbered.Value == "Legacy_3",
			"Chunked name-array number suffix decoded incorrectly");
		const EngineNameResult legacyWideResult = legacyCodec.Decode(1);
		Require(
			legacyWideResult.Ok() && legacyWideResult.Value == "Wi",
			"Chunked wide name entry decoded incorrectly");
		Require(
			legacyCodec.Decode(2).Error == EngineNameError::IndexOutOfRange,
			"Chunked name-array element count did not bound an index");
		writeBytes(legacyNarrow, 0, std::uint32_t{4});
		Require(
			legacyCodec.Decode(0).Error == EngineNameError::HeaderInvalid,
			"Chunked name-array entry identity mismatch was accepted");

		EngineNameProfile invalidProfile = poolProfile;
		invalidProfile.ChunksStart = 3;
		Require(
			!EngineNameCodec(invalidProfile).IsConfigured(),
			"Structurally invalid name profile was accepted");
		EngineNameProfile inaccessibleProfile = poolProfile;
		inaccessibleProfile.StorageAddress = 1;
		Require(
			EngineNameCodec(inaccessibleProfile).Decode(1).Error == EngineNameError::EntryUnavailable,
			"Name codec did not convert an inaccessible storage read into a stable error");
	}

	void TestPropertyCodec()
	{
		using namespace UExplorer::Runtime;

		const auto writeBytes = [](auto& buffer, const std::size_t offset, const auto& value) {
			Require(
				offset <= buffer.size() && sizeof(value) <= buffer.size() - offset,
				"Property codec fixture write exceeded its buffer");
			std::memcpy(buffer.data() + offset, &value, sizeof(value));
		};
		const auto writeSpan = [](auto& buffer, const std::size_t offset, const void* value, const std::size_t size) {
			Require(
				offset <= buffer.size() && size <= buffer.size() - offset,
				"Property codec string fixture write exceeded its buffer");
			std::memcpy(buffer.data() + offset, value, size);
		};

		std::array<std::byte, 64> pool{};
		std::array<std::byte, 96> block{};
		writeBytes(pool, 0, std::int32_t{0});
		writeBytes(pool, 4, std::int32_t{64});
		writeBytes(pool, 16, reinterpret_cast<std::uintptr_t>(block.data()));
		writeBytes(block, 0, static_cast<std::uint16_t>(4u << 6));
		constexpr char noneText[] = "None";
		writeSpan(block, 2, noneText, 4);
		writeBytes(block, 6, static_cast<std::uint16_t>(5u << 6));
		constexpr char actorText[] = "Actor";
		writeSpan(block, 8, actorText, 5);
		writeBytes(block, 14, static_cast<std::uint16_t>(5u << 6));
		constexpr char assetText[] = "Asset";
		writeSpan(block, 16, assetText, 5);

		EngineNameProfile nameProfile{
			.Storage = EngineNameStorageKind::NamePool,
			.StorageAddress = reinterpret_cast<std::uintptr_t>(pool.data()),
			.FNameSize = 8,
			.ComparisonIndexOffset = 0,
			.NumberOffset = 4,
			.BlockOffsetBits = 14,
			.EntryStride = 2,
			.ChunksStart = 16,
			.MaxChunkIndexOffset = 0,
			.ByteCursorOffset = 4,
			.EntryStringOffset = 2,
			.EntryHeaderOffset = 0,
			.EntryLengthShift = 6,
			.Validated = true,
			.Source = "property-codec-name-fixture"
		};
		EngineNameCodec names(nameProfile);
		Require(names.IsConfigured(), "Property codec fixture name profile was rejected");

		PropertyCodecProfile profile{
			.Validated = true,
			.ReflectionLayoutFingerprint = 0x51515151,
			.Source = "ue-x64-property-fixture",
			.DynamicArray = {
				.Validated = true,
				.DataOffset = 0,
				.NumOffset = 8,
				.MaxOffset = 12,
				.HeaderSize = 16
			},
			.Text = {
				.Validated = true,
				.DataPointerOffset = 0,
				.StringOffsetInData = 0,
				.MinimumValueSize = 8
			},
			.WeakObject = {
				.Validated = true,
				.IndexOffset = 0,
				.SerialOffset = 4,
				.ValueSize = 8
			},
			.SoftObject = {
				.Validated = true,
				.AssetPathNameOffsets = {0, 8},
				.AssetPathNameCount = 2,
				.SubPathStringOffset = 16,
				.MinimumValueSize = 32
			},
			.SparseContainer = {
				.Validated = true,
				.ElementsDataOffset = 0,
				.ElementsNumOffset = 8,
				.ElementsMaxOffset = 12,
				.AllocationInlineDataOffset = 16,
				.AllocationSecondaryDataOffset = 32,
				.AllocationNumBitsOffset = 40,
				.AllocationMaxBitsOffset = 44,
				.HeaderSize = 56,
				.InlineBitWordCount = 4
			}
		};
		PropertyCodec codec(names, profile);
		Require(
			codec.IsConfigured()
				&& IsPropertyCodecProfileValid(profile, nameProfile),
			"A complete validated property codec profile was rejected");
		Require(
			std::string(ToString(PropertyValueState::Unsupported)) == "unsupported"
				&& std::string(ToString(PropertyValueState::Unavailable)) == "unavailable"
				&& std::string(ToString(PropertyValueState::Error)) == "error",
			"Property value states are not explicit and stable");

		const auto intDescriptor = std::make_shared<PropertyDescriptor>(PropertyDescriptor{
			.Kind = PropertyKind::Int32,
			.TypeName = "int32",
			.Size = 4
		});
		std::int32_t integer = -17;
		PropertyValue integerValue = codec.Decode(
			reinterpret_cast<std::uintptr_t>(&integer),
			*intDescriptor);
		Require(
			integerValue.State == PropertyValueState::Ok
				&& std::get<std::int64_t>(integerValue.Scalar) == -17,
			"Signed scalar property decoding changed its width or value");
		std::array<std::byte, sizeof(std::int32_t)> encodedInteger{};
		Require(
			codec.EncodeOwned(encodedInteger, *intDescriptor, std::int64_t{125}).Ok(),
			"Owned parameter codec rejected an exact int32 input");
		std::int32_t encodedIntegerValue = 0;
		std::memcpy(&encodedIntegerValue, encodedInteger.data(), sizeof(encodedIntegerValue));
		PropertyDescriptor wrongWidthInteger = *intDescriptor;
		wrongWidthInteger.Size = sizeof(std::int64_t);
		std::array<std::byte, sizeof(std::int64_t)> wrongWidthDestination{};
		Require(
			encodedIntegerValue == 125
				&& codec.EncodeOwned(
					wrongWidthDestination,
					wrongWidthInteger,
					std::int64_t{125}).Error == PropertyEncodeError::DescriptorInvalid,
			"Owned parameter codec ignored the reflected scalar width");
		ReflectedProperty frameProperty{
			.Name = "Health",
			.TypeName = "int32",
			.Kind = PropertyKind::Int32,
			.Offset = 0,
			.Size = sizeof(std::int32_t),
			.ArrayDim = 1,
			.State = ReflectedMemberState::Supported,
			.Descriptor = intDescriptor
		};
		ParamFrame frame;
		Require(
			ParamFrame::Create(sizeof(std::int32_t), frame).Ok()
				&& frame.SetInput(frameProperty, std::int64_t{-33}, codec).Ok(),
			"Owned ProcessEvent frame rejected a bounded trivial input");
		std::int32_t frameValue = 0;
		std::memcpy(&frameValue, frame.Data(), sizeof(frameValue));
		Require(frameValue == -33, "Owned ProcessEvent frame encoded the wrong scalar value");
		PropertyCodecProfile scalarProfile{
			.Validated = true,
			.ReflectionLayoutFingerprint = 0x61616161,
			.Source = "scalar-property-profile"
		};
		PropertyCodec scalarCodec(names, scalarProfile);
		PropertyDescriptor unavailableString{
			.Kind = PropertyKind::String,
			.TypeName = "string",
			.Size = 16
		};
		Require(
			scalarCodec.IsConfigured()
				&& scalarCodec.Supports(PropertyKind::Int32)
				&& scalarCodec.Supports(PropertyKind::Name)
				&& !scalarCodec.Supports(PropertyKind::String)
				&& scalarCodec.Decode(
					reinterpret_cast<std::uintptr_t>(&integer),
					*intDescriptor).State == PropertyValueState::Ok
				&& scalarCodec.Decode(
					reinterpret_cast<std::uintptr_t>(&integer),
					unavailableString).ErrorCode == "PROPERTY_STRING_LAYOUT_UNAVAILABLE",
			"A missing optional value layout blocked scalar decoding or silently enabled strings");
		PropertyCodecProfile dormantInvalidProfile = scalarProfile;
		dormantInvalidProfile.DynamicArray.DataOffset = 0;
		Require(
			!PropertyCodec(names, dormantInvalidProfile).IsConfigured(),
			"Unwitnessed dormant layout fields were accepted as an absent optional profile");

		std::uint8_t boolByte = 0x04;
		PropertyDescriptor boolDescriptor{
			.Kind = PropertyKind::Bool,
			.TypeName = "bool",
			.Size = 1,
			.BoolByteOffset = 0,
			.BoolMask = 0x04
		};
		Require(
			std::get<bool>(codec.Decode(
				reinterpret_cast<std::uintptr_t>(&boolByte),
				boolDescriptor).Scalar),
			"Bitfield bool mask was ignored");

		const std::array<std::uint32_t, 2> actorName{3, 0};
		PropertyDescriptor nameDescriptor{
			.Kind = PropertyKind::Name,
			.TypeName = "FName",
			.Size = 8
		};
		PropertyValue decodedName = codec.Decode(
			reinterpret_cast<std::uintptr_t>(actorName.data()),
			nameDescriptor);
		Require(
			decodedName.State == PropertyValueState::Ok
				&& std::get<std::string>(decodedName.Scalar) == "Actor",
			"FName property bypassed the immutable name codec");

		struct ArrayHeader
		{
			std::uintptr_t Data = 0;
			std::int32_t Num = 0;
			std::int32_t Max = 0;
		};
		static_assert(sizeof(ArrayHeader) == 16);
		std::array<wchar_t, 6> hello{L'H', L'e', L'l', L'l', L'o', L'\0'};
		ArrayHeader stringHeader{
			.Data = reinterpret_cast<std::uintptr_t>(hello.data()),
			.Num = static_cast<std::int32_t>(hello.size()),
			.Max = static_cast<std::int32_t>(hello.size())
		};
		PropertyDescriptor stringDescriptor{
			.Kind = PropertyKind::String,
			.TypeName = "FString",
			.Size = 16
		};
		PropertyValue stringValue = codec.Decode(
			reinterpret_cast<std::uintptr_t>(&stringHeader),
			stringDescriptor);
		Require(
			stringValue.State == PropertyValueState::Ok
				&& std::get<std::string>(stringValue.Scalar) == "Hello",
			"FString was not copied and converted through the bounded UTF-16 codec");

		std::uintptr_t textData = reinterpret_cast<std::uintptr_t>(&stringHeader);
		PropertyDescriptor textDescriptor{
			.Kind = PropertyKind::Text,
			.TypeName = "FText",
			.Size = 8
		};
		PropertyValue textValue = codec.Decode(
			reinterpret_cast<std::uintptr_t>(&textData),
			textDescriptor);
		Require(
			textValue.State == PropertyValueState::Ok
				&& std::get<std::string>(textValue.Scalar) == "Hello",
			"Validated FText layout returned an unresolved placeholder");

		class FixtureReferenceResolver final : public IPropertyReferenceResolver
		{
		public:
			bool ReturnInvalidHandle = false;
			bool ReturnMismatchedAddress = false;
			bool ReturnWrongContext = false;

			std::string_view SessionId() const noexcept override { return "property-fixture"; }
			std::uint64_t ContextGeneration() const noexcept override { return 9; }

			PropertyReferenceResult ResolveAddress(const std::uintptr_t address) override
			{
				return address == 0x1234
					? Success(address)
					: Failure();
			}

			PropertyReferenceResult ResolveWeak(
				const std::int32_t index,
				const std::int32_t serialNumber) override
			{
				return index == 7 && serialNumber == 77
					? Success(0x1234)
					: Failure();
			}

		private:
			PropertyReferenceResult Success(const std::uintptr_t address) const
			{
				return {
					.State = PropertyValueState::Ok,
					.Handle = {
						.SessionId = "property-fixture",
						.ContextGeneration = ReturnInvalidHandle ? 0u : (ReturnWrongContext ? 10u : 9u),
						.Index = 7,
						.SerialNumber = 77,
						.Address = ReturnMismatchedAddress ? address + 8u : address,
						.ClassFingerprint = 0xABCD
					}
				};
			}

			static PropertyReferenceResult Failure()
			{
				return {
					.State = PropertyValueState::Error,
					.ErrorCode = "PROPERTY_REFERENCE_STALE",
					.ErrorMessage = "Fixture reference is stale"
				};
			}
		} resolver;
		PropertyDecodeOptions referenceOptions;
		referenceOptions.ReferenceResolver = &resolver;
		std::uintptr_t objectPointer = 0x1234;
		PropertyDescriptor objectDescriptor{
			.Kind = PropertyKind::Object,
			.TypeName = "UObject*",
			.Size = 8
		};
		PropertyValue objectValue = codec.Decode(
			reinterpret_cast<std::uintptr_t>(&objectPointer),
			objectDescriptor,
			referenceOptions);
		Require(
			objectValue.State == PropertyValueState::Ok
				&& std::get<PropertyObjectReference>(objectValue.Scalar).Handle.SerialNumber == 77,
			"Object property returned a raw address instead of a stable handle");
		Require(
			codec.Decode(
				reinterpret_cast<std::uintptr_t>(&objectPointer),
				objectDescriptor).State == PropertyValueState::Unavailable,
			"Object property succeeded without a stable reference resolver");
		resolver.ReturnInvalidHandle = true;
		Require(
			codec.Decode(
				reinterpret_cast<std::uintptr_t>(&objectPointer),
				objectDescriptor,
				referenceOptions).ErrorCode == "PROPERTY_REFERENCE_RESULT_INVALID",
			"Reference resolver success bypassed the stable-handle envelope checks");
		resolver.ReturnInvalidHandle = false;
		resolver.ReturnMismatchedAddress = true;
		Require(
			codec.Decode(
				reinterpret_cast<std::uintptr_t>(&objectPointer),
				objectDescriptor,
				referenceOptions).ErrorCode == "PROPERTY_REFERENCE_RESULT_INVALID",
			"Reference resolver success returned a handle for a different address");
		resolver.ReturnMismatchedAddress = false;
		resolver.ReturnWrongContext = true;
		Require(
			codec.Decode(
				reinterpret_cast<std::uintptr_t>(&objectPointer),
				objectDescriptor,
				referenceOptions).ErrorCode == "PROPERTY_REFERENCE_RESULT_INVALID",
			"Reference resolver returned a handle from a different context generation");
		resolver.ReturnWrongContext = false;

		std::array<std::int32_t, 2> weakIdentity{7, 77};
		PropertyDescriptor weakDescriptor{
			.Kind = PropertyKind::WeakObject,
			.TypeName = "TWeakObjectPtr<UObject>",
			.Size = 8
		};
		Require(
			codec.Decode(
				reinterpret_cast<std::uintptr_t>(weakIdentity.data()),
				weakDescriptor,
				referenceOptions).State == PropertyValueState::Ok,
			"Weak object index/serial did not resolve to a stable handle");
		weakIdentity[1] = 78;
		Require(
			codec.Decode(
				reinterpret_cast<std::uintptr_t>(weakIdentity.data()),
				weakDescriptor,
				referenceOptions).ErrorCode == "PROPERTY_REFERENCE_STALE",
			"Stale weak object identity was reported as a successful value");

		std::array<wchar_t, 4> subPathText{L'S', L'u', L'b', L'\0'};
		ArrayHeader subPathHeader{
			.Data = reinterpret_cast<std::uintptr_t>(subPathText.data()),
			.Num = static_cast<std::int32_t>(subPathText.size()),
			.Max = static_cast<std::int32_t>(subPathText.size())
		};
		std::array<std::byte, 32> softValueBytes{};
		const std::array<std::uint32_t, 2> assetName{3, 0};
		const std::array<std::uint32_t, 2> objectName{7, 0};
		writeBytes(softValueBytes, 0, assetName);
		writeBytes(softValueBytes, 8, objectName);
		writeBytes(softValueBytes, 16, subPathHeader);
		PropertyDescriptor softDescriptor{
			.Kind = PropertyKind::SoftObject,
			.TypeName = "TSoftObjectPtr<UObject>",
			.Size = 32
		};
		PropertyValue softValue = codec.Decode(
			reinterpret_cast<std::uintptr_t>(softValueBytes.data()),
			softDescriptor);
		Require(
			softValue.State == PropertyValueState::Ok
				&& std::get<std::string>(softValue.Scalar) == "Actor.Asset:Sub",
			"Soft object path was guessed as weak index/serial or decoded out of order");

		std::array<std::int32_t, 2> structData{11, 22};
		PropertyDescriptor structDescriptor{
			.Kind = PropertyKind::Struct,
			.TypeName = "FixtureStruct",
			.Size = 8,
			.Fields = {
				{.Name = "A", .Offset = 0, .Descriptor = intDescriptor},
				{.Name = "B", .Offset = 4, .Descriptor = intDescriptor}
			}
		};
		PropertyValue structValue = codec.Decode(
			reinterpret_cast<std::uintptr_t>(structData.data()),
			structDescriptor);
		Require(
			structValue.State == PropertyValueState::Ok
				&& structValue.Children.size() == 2
				&& structValue.Children[1].Label == "B",
			"Struct fields were not bounded and labeled deterministically");
		PropertyDescriptor cyclicDescriptor{
			.Kind = PropertyKind::Struct,
			.TypeName = "Cycle",
			.Size = 4
		};
		cyclicDescriptor.Fields.push_back({
			.Name = "Self",
			.Offset = 0,
			.Descriptor = std::shared_ptr<const PropertyDescriptor>(
				&cyclicDescriptor,
				[](const PropertyDescriptor*) {})
		});
		Require(
			codec.Decode(
				reinterpret_cast<std::uintptr_t>(&integer),
				cyclicDescriptor).ErrorCode == "PROPERTY_RECURSION_CYCLE",
			"Recursive property descriptor/address pair bypassed the cycle guard");
		PropertyDescriptor cyclicArrayDescriptor{
			.Kind = PropertyKind::Array,
			.TypeName = "TArray<Self>",
			.Size = 16,
			.ElementStride = 16
		};
		cyclicArrayDescriptor.Element = std::shared_ptr<const PropertyDescriptor>(
			&cyclicArrayDescriptor,
			[](const PropertyDescriptor*) {});
		ArrayHeader cyclicArrayHeader{
			.Data = 0,
			.Num = 1,
			.Max = 1
		};
		cyclicArrayHeader.Data = reinterpret_cast<std::uintptr_t>(&cyclicArrayHeader);
		Require(
			codec.Decode(
				reinterpret_cast<std::uintptr_t>(&cyclicArrayHeader),
				cyclicArrayDescriptor).ErrorCode == "PROPERTY_RECURSION_CYCLE",
			"Recursive array descriptor/address pair bypassed the generic cycle guard");

		std::array<std::int32_t, 3> arrayData{3, 5, 8};
		ArrayHeader arrayHeader{
			.Data = reinterpret_cast<std::uintptr_t>(arrayData.data()),
			.Num = static_cast<std::int32_t>(arrayData.size()),
			.Max = static_cast<std::int32_t>(arrayData.size())
		};
		PropertyDescriptor arrayDescriptor{
			.Kind = PropertyKind::Array,
			.TypeName = "TArray<int32>",
			.Size = 16,
			.ElementStride = 4,
			.Element = intDescriptor
		};
		PropertyDecodeOptions previewOptions;
		previewOptions.Limits.MaxContainerElements = 2;
		PropertyValue arrayValue = codec.Decode(
			reinterpret_cast<std::uintptr_t>(&arrayHeader),
			arrayDescriptor,
			previewOptions);
		Require(
			arrayValue.State == PropertyValueState::Ok
				&& arrayValue.TotalCount == 3
				&& arrayValue.Truncated
				&& arrayValue.Children.size() == 2,
			"Array preview did not expose exact total/truncation bounds");
		PropertyDecodeOptions nodeBudgetOptions;
		nodeBudgetOptions.Limits.MaxTotalNodes = 2;
		PropertyValue budgetedArray = codec.Decode(
			reinterpret_cast<std::uintptr_t>(&arrayHeader),
			arrayDescriptor,
			nodeBudgetOptions);
		Require(
			budgetedArray.State == PropertyValueState::Error
				&& budgetedArray.ErrorCode == "PROPERTY_NODE_BUDGET_EXCEEDED"
				&& budgetedArray.Truncated
				&& budgetedArray.Children.size() == 1,
			"Total property node budget did not stop and truncate the output value tree");
		ArrayHeader invalidArray = arrayHeader;
		invalidArray.Max = 1;
		Require(
			codec.Decode(
				reinterpret_cast<std::uintptr_t>(&invalidArray),
				arrayDescriptor).ErrorCode == "PROPERTY_CONTAINER_HEADER_INVALID",
			"Array with Num greater than Max was accepted");

		std::array<std::byte, 48> setSlots{};
		writeBytes(setSlots, 0, std::int32_t{10});
		writeBytes(setSlots, 16, std::int32_t{20});
		writeBytes(setSlots, 32, std::int32_t{30});
		std::array<std::byte, 56> setHeader{};
		writeBytes(setHeader, 0, reinterpret_cast<std::uintptr_t>(setSlots.data()));
		writeBytes(setHeader, 8, std::int32_t{3});
		writeBytes(setHeader, 12, std::int32_t{3});
		writeBytes(setHeader, 16, std::uint32_t{0b101});
		writeBytes(setHeader, 32, std::uintptr_t{0});
		writeBytes(setHeader, 40, std::int32_t{3});
		writeBytes(setHeader, 44, std::int32_t{128});
		PropertyDescriptor setDescriptor{
			.Kind = PropertyKind::Set,
			.TypeName = "TSet<int32>",
			.Size = 56,
			.ElementStride = 16,
			.ElementValueOffset = 0,
			.Element = intDescriptor
		};
		PropertyValue setValue = codec.Decode(
			reinterpret_cast<std::uintptr_t>(setHeader.data()),
			setDescriptor);
		Require(
			setValue.State == PropertyValueState::Ok
				&& setValue.TotalCount == 2
				&& setValue.Children.size() == 2,
			"Sparse set allocation bits were ignored or inactive slots were decoded");
		PropertyDecodeOptions sparseNodeBudgetOptions;
		sparseNodeBudgetOptions.Limits.MaxTotalNodes = 2;
		PropertyValue budgetedSet = codec.Decode(
			reinterpret_cast<std::uintptr_t>(setHeader.data()),
			setDescriptor,
			sparseNodeBudgetOptions);
		Require(
			budgetedSet.State == PropertyValueState::Error
				&& budgetedSet.Truncated
				&& budgetedSet.Children.size() == 1
				&& budgetedSet.Children[0].Children.empty(),
			"Sparse container item wrappers bypassed the total property node budget");

		std::array<std::byte, 48> mapSlots{};
		writeBytes(mapSlots, 0, std::int32_t{1});
		writeBytes(mapSlots, 4, std::int32_t{100});
		writeBytes(mapSlots, 32, std::int32_t{2});
		writeBytes(mapSlots, 36, std::int32_t{200});
		std::array<std::byte, 56> mapHeader = setHeader;
		writeBytes(mapHeader, 0, reinterpret_cast<std::uintptr_t>(mapSlots.data()));
		PropertyDescriptor mapDescriptor{
			.Kind = PropertyKind::Map,
			.TypeName = "TMap<int32,int32>",
			.Size = 56,
			.ElementStride = 16,
			.MapKeyOffset = 0,
			.MapValueOffset = 4,
			.Key = intDescriptor,
			.Mapped = intDescriptor
		};
		PropertyValue mapValue = codec.Decode(
			reinterpret_cast<std::uintptr_t>(mapHeader.data()),
			mapDescriptor);
		Require(
			mapValue.State == PropertyValueState::Ok
				&& mapValue.TotalCount == 2
				&& mapValue.Children[0].Children.size() == 2,
			"Sparse map key/value pairs were not decoded through bounded descriptors");
		PropertyDescriptor overlappingMapDescriptor = mapDescriptor;
		overlappingMapDescriptor.MapValueOffset = 2;
		Require(
			codec.Decode(
				reinterpret_cast<std::uintptr_t>(mapHeader.data()),
				overlappingMapDescriptor).ErrorCode == "PROPERTY_DESCRIPTOR_INVALID",
			"Overlapping sparse map key/value ranges were accepted");

		PropertyDescriptor enumDescriptor{
			.Kind = PropertyKind::Enum,
			.TypeName = "EFixture",
			.Size = 4,
			.Element = intDescriptor,
			.EnumEntries = {
				{.RawValue = 2, .Name = "EFixture::Two"}
			}
		};
		std::int32_t enumRaw = 2;
		PropertyValue enumValue = codec.Decode(
			reinterpret_cast<std::uintptr_t>(&enumRaw),
			enumDescriptor);
		Require(
			enumValue.State == PropertyValueState::Ok
				&& enumValue.DisplayName == "EFixture::Two",
			"Enum underlying value was not preserved with an exact symbolic match");

		PropertyDescriptor delegateDescriptor{
			.Kind = PropertyKind::Delegate,
			.TypeName = "FScriptDelegate",
			.Size = 16
		};
		Require(
			codec.Decode(
				reinterpret_cast<std::uintptr_t>(&stringHeader),
				delegateDescriptor).State == PropertyValueState::Unsupported,
			"Unimplemented delegate codec was reported as a successful value");
		PropertyCodec unavailableCodec(names, {});
		Require(
			!unavailableCodec.IsConfigured()
				&& unavailableCodec.Decode(
					reinterpret_cast<std::uintptr_t>(&stringHeader),
					stringDescriptor).State == PropertyValueState::Unavailable,
			"Missing property layout profile silently fell back to the x64 fixture layout");
		PropertyCodecProfile overlappingProfile = profile;
		overlappingProfile.DynamicArray.NumOffset = 0;
		PropertyCodec overlappingCodec(names, overlappingProfile);
		Require(
			!overlappingCodec.IsConfigured()
				&& overlappingCodec.Decode(
					reinterpret_cast<std::uintptr_t>(&integer),
					*intDescriptor).ErrorCode == "PROPERTY_CODEC_NOT_CONFIGURED",
			"Overlapping layout fields configured a partially usable property codec");

	}

	void TestReflectionLayout()
	{
		using namespace UExplorer::Runtime;

		const auto writeBytes = [](auto& buffer, const std::size_t offset, const auto& value) {
			Require(
				offset <= buffer.size() && sizeof(value) <= buffer.size() - offset,
				"Reflection layout fixture write exceeded its buffer");
			std::memcpy(buffer.data() + offset, &value, sizeof(value));
		};
		const auto writeSpan = [](auto& buffer, const std::size_t offset, const void* value, const std::size_t size) {
			Require(
				offset <= buffer.size() && size <= buffer.size() - offset,
				"Reflection name fixture write exceeded its buffer");
			std::memcpy(buffer.data() + offset, value, size);
		};

		std::array<std::byte, 64> pool{};
		std::array<std::byte, 96> block{};
		writeBytes(pool, 0, std::int32_t{0});
		writeBytes(pool, 4, std::int32_t{64});
		writeBytes(pool, 16, reinterpret_cast<std::uintptr_t>(block.data()));
		writeBytes(block, 0, static_cast<std::uint16_t>(4u << 6));
		constexpr char noneText[] = "None";
		writeSpan(block, 2, noneText, 4);
		writeBytes(block, 6, static_cast<std::uint16_t>(5u << 6));
		constexpr char actorText[] = "Actor";
		writeSpan(block, 8, actorText, 5);

		EngineNameProfile nameProfile{
			.Storage = EngineNameStorageKind::NamePool,
			.StorageAddress = reinterpret_cast<std::uintptr_t>(pool.data()),
			.FNameSize = 8,
			.ComparisonIndexOffset = 0,
			.NumberOffset = 4,
			.BlockOffsetBits = 14,
			.EntryStride = 2,
			.ChunksStart = 16,
			.MaxChunkIndexOffset = 0,
			.ByteCursorOffset = 4,
			.EntryStringOffset = 2,
			.EntryHeaderOffset = 0,
			.EntryLengthShift = 6,
			.Validated = true,
			.Source = "reflection-layout-name-fixture"
		};
		EngineNameCodec names(nameProfile);
		Require(names.IsConfigured(), "Reflection layout name codec fixture was rejected");

		ReflectionLayoutCandidate candidate{
			.ContextGeneration = 51,
			.PropertySystem = ReflectionPropertySystem::FProperty,
			.Source = "synthetic-fproperty-semantic-witness",
			.Fields = {
				{ReflectionField::StructSuper, 0, 64, "synthetic"},
				{ReflectionField::StructChildProperties, 8, 64, "synthetic"},
				{ReflectionField::StructPropertiesSize, 16, 64, "synthetic"},
				{ReflectionField::StructMinAlignment, 20, 64, "synthetic"},
				{ReflectionField::FFieldClass, 0, 64, "synthetic"},
				{ReflectionField::FFieldNext, 8, 64, "synthetic"},
				{ReflectionField::FFieldName, 16, 64, "synthetic"},
				{ReflectionField::FFieldClassCastFlags, 0, 16, "synthetic"},
				{ReflectionField::PropertyArrayDim, 24, 64, "synthetic"},
				{ReflectionField::PropertyElementSize, 28, 64, "synthetic"},
				{ReflectionField::PropertyFlags, 32, 64, "synthetic"},
				{ReflectionField::PropertyOffset, 40, 64, "synthetic"},
				{ReflectionField::BoolFieldSize, 44, 64, "synthetic"},
				{ReflectionField::BoolByteOffset, 45, 64, "synthetic"},
				{ReflectionField::BoolByteMask, 46, 64, "synthetic"},
				{ReflectionField::BoolFieldMask, 47, 64, "synthetic"},
				{ReflectionField::BytePropertyEnum, 48, 64, "synthetic"},
				{ReflectionField::ObjectPropertyClass, 48, 64, "synthetic"},
				{ReflectionField::StructPropertyStruct, 48, 64, "synthetic"},
				{ReflectionField::ArrayPropertyInner, 48, 64, "synthetic"},
				{ReflectionField::MapPropertyKey, 48, 72, "synthetic"},
				{ReflectionField::MapPropertyValue, 56, 72, "synthetic"},
				{ReflectionField::SetPropertyElement, 48, 64, "synthetic"},
				{ReflectionField::EnumPropertyUnderlying, 48, 72, "synthetic"},
				{ReflectionField::EnumPropertyEnum, 56, 72, "synthetic"}
			}
		};
		std::vector<std::array<std::byte, 80>> witnessStorage(candidate.Fields.size());
		std::byte readableTarget{};
		const std::uintptr_t readableTargetAddress =
			reinterpret_cast<std::uintptr_t>(&readableTarget);
		for (std::size_t index = 0; index < candidate.Fields.size(); ++index)
		{
			const ReflectionFieldCandidate& field = candidate.Fields[index];
			ReflectionFieldWitness witness{
				.Id = std::string("witness-") + ToString(field.Field),
				.Field = field.Field,
				.BaseAddress = reinterpret_cast<std::uintptr_t>(witnessStorage[index].data())
			};
			switch (ValueKindFor(field.Field))
			{
			case ReflectionFieldValueKind::Pointer:
				writeBytes(witnessStorage[index], field.Offset, readableTargetAddress);
				witness.Expected = static_cast<std::uint64_t>(readableTargetAddress);
				break;
			case ReflectionFieldValueKind::UInt8:
			{
				const std::uint8_t value = field.Field == ReflectionField::BoolByteOffset ? 0 : 1;
				writeBytes(witnessStorage[index], field.Offset, value);
				witness.Expected = static_cast<std::uint64_t>(value);
				break;
			}
			case ReflectionFieldValueKind::Int32:
			{
				std::int32_t value = 16;
				if (field.Field == ReflectionField::PropertyArrayDim)
					value = 1;
				else if (field.Field == ReflectionField::PropertyElementSize)
					value = 8;
				else if (field.Field == ReflectionField::StructPropertiesSize)
					value = 256;
				else if (field.Field == ReflectionField::StructMinAlignment)
					value = 8;
				writeBytes(witnessStorage[index], field.Offset, value);
				witness.Expected = static_cast<std::uint64_t>(value);
				break;
			}
			case ReflectionFieldValueKind::UInt64:
				writeBytes(witnessStorage[index], field.Offset, std::uint64_t{1});
				witness.Expected = std::uint64_t{1};
				break;
			case ReflectionFieldValueKind::FName:
			{
				const std::array<std::uint32_t, 2> actorName{3, 0};
				writeBytes(witnessStorage[index], field.Offset, actorName);
				witness.Expected = std::string("Actor");
				break;
			}
			}
			candidate.Witnesses.push_back(std::move(witness));
		}

		const ReflectionLayoutValidationResult validated =
			ValidateReflectionLayout(candidate, names);
		Require(
			validated.Ok()
				&& validated.Layout->Fingerprint() != 0
				&& validated.Layout->ContextGeneration() == 51
				&& validated.Layout->PropertySystem() == ReflectionPropertySystem::FProperty
				&& validated.Layout->Fields().size() == candidate.Fields.size(),
			"Complete FProperty layout witnesses did not publish an immutable layout");
		const ReflectionLayoutValidationResult repeated =
			ValidateReflectionLayout(candidate, names);
		Require(
			repeated.Ok()
				&& repeated.Layout->Fingerprint() == validated.Layout->Fingerprint(),
			"Identical reflection layouts produced different fingerprints");
		Require(
			IsReflectionLayoutValid(*validated.Layout, 51)
				&& !IsReflectionLayoutValid(*validated.Layout, 52),
			"Reflection layout generation binding was not exact");

		ReflectionLayoutCandidate missingField = candidate;
		missingField.Fields.pop_back();
		Require(
			ValidateReflectionLayout(missingField, names).Error
				== ReflectionValidationError::MissingRequiredField,
			"A partial reflection field set was accepted");
		ReflectionLayoutCandidate unexpectedField = candidate;
		unexpectedField.Fields.front().Field = ReflectionField::StructChildren;
		Require(
			ValidateReflectionLayout(unexpectedField, names).Error
				== ReflectionValidationError::UnexpectedField,
			"A field from the other property-system contract was accepted");
		ReflectionLayoutCandidate duplicateField = candidate;
		duplicateField.Fields[1].Field = ReflectionField::StructSuper;
		Require(
			ValidateReflectionLayout(duplicateField, names).Error
				== ReflectionValidationError::DuplicateField,
			"A duplicate reflection field was accepted");
		ReflectionLayoutCandidate overlapping = candidate;
		for (ReflectionFieldCandidate& field : overlapping.Fields)
		{
			if (field.Field == ReflectionField::PropertyElementSize)
				field.Offset = 24;
		}
		Require(
			ValidateReflectionLayout(overlapping, names).Error
				== ReflectionValidationError::FieldOverlap,
			"Overlapping reflection fields were accepted");
		ReflectionLayoutCandidate inconsistentRecord = candidate;
		for (ReflectionFieldCandidate& field : inconsistentRecord.Fields)
		{
			if (field.Field == ReflectionField::PropertyElementSize)
				field.ContainerSize = 72;
		}
		Require(
			ValidateReflectionLayout(inconsistentRecord, names).Error
				== ReflectionValidationError::FieldLayoutInvalid,
			"One record kind accepted conflicting container sizes");
		std::size_t minAlignmentIndex = candidate.Fields.size();
		for (std::size_t index = 0; index < candidate.Fields.size(); ++index)
		{
			if (candidate.Fields[index].Field == ReflectionField::StructMinAlignment)
				minAlignmentIndex = index;
		}
		Require(
			minAlignmentIndex < candidate.Fields.size(),
			"Reflection layout fixture omitted the UStruct minimum alignment field");
		const std::int32_t invalidAlignment = 3;
		writeBytes(
			witnessStorage[minAlignmentIndex],
			candidate.Fields[minAlignmentIndex].Offset,
			invalidAlignment);
		ReflectionLayoutCandidate badAlignment = candidate;
		badAlignment.Witnesses[minAlignmentIndex].Expected =
			static_cast<std::uint64_t>(invalidAlignment);
		Require(
			ValidateReflectionLayout(badAlignment, names).Error
				== ReflectionValidationError::WitnessSemanticInvalid,
			"A non-power-of-two UStruct minimum alignment was accepted");
		const std::int32_t validAlignment = 8;
		writeBytes(
			witnessStorage[minAlignmentIndex],
			candidate.Fields[minAlignmentIndex].Offset,
			validAlignment);
		ReflectionLayoutCandidate corruptWitness = candidate;
		corruptWitness.Witnesses.front().Expected = std::uint64_t{0};
		const ReflectionLayoutValidationResult corruptResult =
			ValidateReflectionLayout(corruptWitness, names);
		Require(
			corruptResult.Error == ReflectionValidationError::WitnessValueMismatch
				&& !corruptResult.FieldReports.empty()
				&& corruptResult.FieldReports.front().ReasonCode
					== ToString(ReflectionValidationError::WitnessValueMismatch),
			"A reflection witness with mismatched bytes was accepted");
		ReflectionLayoutCandidate wrongName = candidate;
		for (ReflectionFieldWitness& witness : wrongName.Witnesses)
		{
			if (witness.Field == ReflectionField::FFieldName)
				witness.Expected = std::string("Pawn");
		}
		Require(
			ValidateReflectionLayout(wrongName, names).Error
				== ReflectionValidationError::NameWitnessMismatch,
			"A reflection FName witness with the wrong semantic name was accepted");
		ReflectionLayoutCandidate unavailable = candidate;
		unavailable.PropertySystem = ReflectionPropertySystem::Unavailable;
		Require(
			ValidateReflectionLayout(unavailable, names).Error
				== ReflectionValidationError::PropertySystemUnavailable,
			"An unknown reflection property system was guessed");

		const auto makePropertyProfile = [](const std::uint64_t fingerprint) {
			return PropertyCodecProfile{
				.Validated = true,
				.ReflectionLayoutFingerprint = fingerprint,
				.Source = "reflection-bound-property-fixture",
				.DynamicArray = {true, 0, 8, 12, 16},
				.Text = {true, 0, 0, 8},
				.WeakObject = {true, 0, 4, 8},
				.SoftObject = {true, {0, 8}, 2, 16, 32},
				.SparseContainer = {true, 0, 8, 12, 16, 32, 40, 44, 56, 4}
			};
		};
		class FacadeIdentitySource final : public IHandleIdentitySource
		{
		public:
			FacadeIdentitySource() noexcept : m_ExecutionThreadId(GetCurrentThreadId()) {}
			std::uint64_t ContextGeneration() const noexcept override { return 51; }
			bool IsCurrentExecutionThreadValid() const noexcept override
			{
				return GetCurrentThreadId() == m_ExecutionThreadId;
			}
			bool TryReadObject(std::int32_t, ObjectIdentity&) override { return false; }
			bool TryReadFunction(std::int32_t, FunctionIdentity&) override { return false; }

		private:
			DWORD m_ExecutionThreadId = 0;
		} facadeIdentity;
		EngineContextBuilder contextBuilder(51);
		contextBuilder.SetIdentity(0x140000000, 0x140100000, 4242, 0, "Fixture", "5.4");
		contextBuilder.SetNameProfile(nameProfile);
		contextBuilder.AddOffset({
			.Name = "process_event.index",
			.Value = 64,
			.Required = true,
			.State = ValidationState::Validated,
			.Source = "type-snapshot-fixture"
		});
		contextBuilder.AddOffset({
			.Name = "process_event.offset",
			.Value = 0x1000,
			.Required = true,
			.State = ValidationState::Validated,
			.Source = "type-snapshot-fixture"
		});
		const std::shared_ptr<const EngineContext> context = contextBuilder.Build();
		class SyntheticReflectionCandidateSource final : public IReflectionCandidateSource
		{
		public:
			explicit SyntheticReflectionCandidateSource(ReflectionLayoutCandidate candidate)
				: m_Template(std::move(candidate))
			{
			}

			std::uint64_t ContextGeneration() const noexcept override
			{
				return m_Template.ContextGeneration;
			}

			bool IsConfigured() const noexcept override
			{
				return m_Template.ContextGeneration != 0
					&& m_Template.PropertySystem != ReflectionPropertySystem::Unavailable
					&& !m_Template.Source.empty();
			}

			bool IsCurrentExecutionThreadValid() const noexcept override
			{
				return m_ThreadValid;
			}

			ReflectionCandidateSourceBeginResult Begin() noexcept override
			{
				m_Cursor = 0;
				m_Active = true;
				m_PreparationEmitted = false;
				return {
					.PropertySystem = m_Template.PropertySystem,
					.Source = m_Template.Source
				};
			}

			ReflectionCandidateSourceStepResult CaptureNext() noexcept override
			{
				if (m_Active && !m_PreparationEmitted)
				{
					m_PreparationEmitted = true;
					return {.Progressed = true};
				}
				if (!m_Active || m_Cursor >= m_Template.Fields.size())
				{
					return {
						.Error = ReflectionCandidateSourceError::ContractViolation
					};
				}
				const ReflectionField field = m_Template.Fields[m_Cursor].Field;
				ReflectionCandidateEvidence evidence{
					.Field = m_Template.Fields[m_Cursor]
				};
				for (const ReflectionFieldWitness& witness : m_Template.Witnesses)
				{
					if (witness.Field == field)
						evidence.Witnesses.push_back(witness);
				}
				++m_Cursor;
				return {
					.Progressed = true,
					.Complete = m_Cursor == m_Template.Fields.size(),
					.Evidence = std::move(evidence)
				};
			}

			bool ValidateDependencies() noexcept override
			{
				return m_Active && m_DependenciesValid;
			}

			void Cancel() noexcept override
			{
				m_Active = false;
				m_Cursor = 0;
			}

			void SetDependenciesValid(const bool valid) noexcept
			{
				m_DependenciesValid = valid;
			}

		private:
			ReflectionLayoutCandidate m_Template;
			std::size_t m_Cursor = 0;
			bool m_Active = false;
			bool m_PreparationEmitted = false;
			bool m_ThreadValid = true;
			bool m_DependenciesValid = true;
		};

		SyntheticReflectionCandidateSource captureSource(candidate);
		EngineFacade captureFacade(context, "reflection-capture", facadeIdentity);
		Require(
			captureFacade.ConfigureReflectionCapture(captureSource),
			"EngineFacade rejected a bounded reflection candidate source");
		ReflectionLayoutCapture* reflectionCapture = captureFacade.ReflectionCapture();
		Require(
			reflectionCapture
				&& reflectionCapture->RequestCapture() == ReflectionLayoutCaptureError::None
				&& reflectionCapture->RequestCapture() == ReflectionLayoutCaptureError::Busy,
			"Reflection capture request admission was not single-owner");
		const ReflectionLayoutPumpResult beginPump = reflectionCapture->Pump(1);
		Require(
			beginPump.Status == ReflectionLayoutPumpStatus::Progress
				&& beginPump.WorkConsumed == 1
				&& beginPump.MoreWorkPending
				&& !captureFacade.Reflection(),
			"Reflection capture begin step published partial evidence");
		const ReflectionLayoutPumpResult publishPump =
			reflectionCapture->Pump(ReflectionLayoutCapture::kMaxPumpBudget);
		const ReflectionLayoutCaptureDiagnostics captureDiagnostics =
			reflectionCapture->Diagnostics();
		Require(
			publishPump.Status == ReflectionLayoutPumpStatus::Published
				&& publishPump.WorkConsumed == candidate.Fields.size() + 3
				&& !publishPump.MoreWorkPending
				&& captureDiagnostics.State == ReflectionLayoutCaptureState::Completed
				&& captureDiagnostics.Error == ReflectionLayoutCaptureError::None
				&& captureDiagnostics.CapturedFields == candidate.Fields.size()
				&& captureDiagnostics.CapturedWitnesses == candidate.Witnesses.size()
				&& captureDiagnostics.SourceSteps == candidate.Fields.size() + 1
				&& captureFacade.Reflection()
				&& captureFacade.Reflection()->Layout->Fingerprint()
					== validated.Layout->Fingerprint()
				&& reflectionCapture->RequestCapture()
					== ReflectionLayoutCaptureError::AlreadyConfigured
				&& captureFacade.Stop(),
			"Bounded reflection evidence was not validated and atomically published");

		ReflectionLayoutCandidate missingWitnessCandidate = candidate;
		missingWitnessCandidate.Witnesses.erase(
			std::remove_if(
				missingWitnessCandidate.Witnesses.begin(),
				missingWitnessCandidate.Witnesses.end(),
				[&](const ReflectionFieldWitness& witness) {
					return witness.Field == missingWitnessCandidate.Fields.front().Field;
				}),
			missingWitnessCandidate.Witnesses.end());
		SyntheticReflectionCandidateSource brokenSource(std::move(missingWitnessCandidate));
		EngineFacade brokenCaptureFacade(context, "reflection-capture-broken", facadeIdentity);
		Require(
			brokenCaptureFacade.ConfigureReflectionCapture(brokenSource),
			"Broken reflection source fixture could not reach capture validation");
		ReflectionLayoutCapture* brokenCapture = brokenCaptureFacade.ReflectionCapture();
		Require(
			brokenCapture
				&& brokenCapture->RequestCapture() == ReflectionLayoutCaptureError::None
				&& brokenCapture->Pump(3).Status == ReflectionLayoutPumpStatus::Failed
				&& brokenCapture->Diagnostics().Error
					== ReflectionLayoutCaptureError::SourceContractViolation
				&& brokenCapture->Diagnostics().SourceError
					== ReflectionCandidateSourceError::ContractViolation
				&& !brokenCaptureFacade.Reflection()
				&& brokenCaptureFacade.Stop(),
			"A source step without a same-field witness escaped fail-closed capture");

		SyntheticReflectionCandidateSource changingSource(candidate);
		EngineFacade changingCaptureFacade(context, "reflection-capture-changing", facadeIdentity);
		Require(
			changingCaptureFacade.ConfigureReflectionCapture(changingSource),
			"Changing reflection source fixture could not be configured");
		ReflectionLayoutCapture* changingCapture = changingCaptureFacade.ReflectionCapture();
		Require(
			changingCapture
				&& changingCapture->RequestCapture() == ReflectionLayoutCaptureError::None
				&& changingCapture->Pump(2).Status == ReflectionLayoutPumpStatus::Progress,
			"Changing reflection source did not enter bounded capture");
		changingSource.SetDependenciesValid(false);
		const ReflectionLayoutPumpResult changedPump = changingCapture->Pump(
			ReflectionLayoutCapture::kMaxPumpBudget);
		Require(
			changedPump.Status == ReflectionLayoutPumpStatus::Failed
				&& changingCapture->Diagnostics().Error
					== ReflectionLayoutCaptureError::DependencyChanged
				&& !changingCaptureFacade.Reflection()
				&& changingCaptureFacade.Stop(),
			"A changed reflection dependency was published after final validation");

		EngineFacade wrongThreadFacade(context, "reflection-wrong-thread", facadeIdentity);
		const bool wrongThreadConfigured = std::async(
			std::launch::async,
			[&]() {
				return wrongThreadFacade.ConfigureReflectionLayout(validated.Layout);
			}).get();
		Require(
			!wrongThreadConfigured
				&& !wrongThreadFacade.Reflection()
				&& wrongThreadFacade.Stop(),
			"Reflection validation was published from a different execution thread");

		EngineFacade facade(context, "reflection-facade", facadeIdentity);
		PropertyCodecProfile profile = makePropertyProfile(validated.Layout->Fingerprint());
		PropertyCodecProfile mismatchedProfile = profile;
		mismatchedProfile.ReflectionLayoutFingerprint ^= 1;
		Require(
			!facade.ConfigurePropertyCodec(profile)
				&& !facade.Reflection(),
			"A property codec was published before its reflection layout");
		Require(
			facade.ConfigureReflectionLayout(validated.Layout),
			"EngineFacade rejected a complete reflection layout");
		const std::shared_ptr<const ReflectionRuntimeSnapshot> layoutOnly = facade.Reflection();
		Require(
			layoutOnly
				&& layoutOnly->IsLayoutConfigured(51)
				&& !layoutOnly->IsPropertyCodecConfigured(51)
				&& !layoutOnly->IsConfigured(51)
				&& !facade.Properties()
				&& !facade.ConfigurePropertyCodec(mismatchedProfile)
				&& facade.Reflection() == layoutOnly,
			"Layout-only reflection publication was not immutable or fingerprint-bound");

		RuntimeProbes layoutOnlyProbes;
		layoutOnlyProbes.SafeMemoryEnabled = true;
		layoutOnlyProbes.Reflection = layoutOnly;
		const auto layoutOnlyCapabilities = BuildCoreCapabilities(*context, layoutOnlyProbes);
		Require(
			layoutOnlyCapabilities->IsAvailable("engine.reflection")
				&& !layoutOnlyCapabilities->IsAvailable("engine.property_codec"),
			"A validated layout did not open reflection independently from property decoding");

		Require(
			facade.ConfigurePropertyCodec(profile),
			"EngineFacade rejected a property codec matching the published layout");
		const std::shared_ptr<const ReflectionRuntimeSnapshot> retained = facade.Reflection();
		const std::shared_ptr<const PropertyCodec> retainedCodec = facade.Properties();
		Require(
			retained
				&& retained != layoutOnly
				&& layoutOnly->IsLayoutConfigured(51)
				&& !layoutOnly->Properties
				&& retained->IsConfigured(51)
				&& retained->IsPropertyCodecConfigured(51)
				&& retainedCodec == retained->Properties
				&& !facade.ConfigureReflectionLayout(validated.Layout)
				&& !facade.ConfigurePropertyCodec(profile),
			"EngineFacade did not atomically upgrade one immutable reflection runtime snapshot");

		RuntimeProbes probes;
		probes.SafeMemoryEnabled = true;
		probes.Reflection = retained;
		const auto capabilities = BuildCoreCapabilities(*context, probes);
		Require(
			capabilities->IsAvailable("engine.reflection")
				&& capabilities->IsAvailable("engine.property_codec"),
			"A validated reflection runtime snapshot did not open its exact capabilities");

		const auto makeHandle = [](const std::int32_t index) {
			return ObjectHandle{
				.SessionId = "reflection-facade",
				.ContextGeneration = 51,
				.Index = index,
				.SerialNumber = 100 + index,
				.Address = static_cast<std::uintptr_t>(0x10000 + index * 0x100),
				.ClassFingerprint = static_cast<std::uint64_t>(0xABC000 + index)
			};
		};
		std::vector<ObjectHandle> handles;
		for (std::int32_t index = 0; index < 9; ++index)
			handles.push_back(makeHandle(index));
		auto objectSnapshot = std::make_shared<EngineSnapshot>(EngineSnapshot{
			.SessionId = "reflection-facade",
			.ContextGeneration = 51,
			.Generation = 7,
			.CapturedAtMonotonicUs = 100,
			.CaptureDurationUs = 10,
			.SourceObjectCount = 9,
			.SkippedSlots = 0,
			.Objects = {
				{handles[0], "/Script/Fixture", "/Script/Fixture", "/Script/CoreUObject.Package", "/Script/Fixture", EngineObjectKind::Package},
				{handles[1], "Base", "/Script/Fixture.Base", "/Script/CoreUObject.Class", "/Script/Fixture", EngineObjectKind::Class},
				{handles[2], "Derived", "/Script/Fixture.Derived", "/Script/CoreUObject.Class", "/Script/Fixture", EngineObjectKind::Class},
				{handles[3], "Vector", "/Script/Fixture.Vector", "/Script/CoreUObject.ScriptStruct", "/Script/Fixture", EngineObjectKind::Struct},
				{handles[4], "Mode", "/Script/Fixture.Mode", "/Script/CoreUObject.Enum", "/Script/Fixture", EngineObjectKind::Enum},
				{handles[5], "BaseOnly", "/Script/Fixture.Base.BaseOnly", "/Script/CoreUObject.Function", "/Script/Fixture", EngineObjectKind::Function},
				{handles[6], "DerivedOnly", "/Script/Fixture.Derived.DerivedOnly", "/Script/CoreUObject.Function", "/Script/Fixture", EngineObjectKind::Function},
				{handles[7], "Default__Base", "/Script/Fixture.Default__Base", "/Script/Fixture.Base", "/Script/Fixture", EngineObjectKind::Object},
				{handles[8], "Default__Derived", "/Script/Fixture.Default__Derived", "/Script/Fixture.Derived", "/Script/Fixture", EngineObjectKind::Object}
			}
		});
		const SnapshotPublishResult objectPublished = facade.Snapshots().Publish(*objectSnapshot);
		Require(
			objectPublished.Ok(),
			"Type snapshot fixture object generation was rejected");

		const auto makeDescriptor = [](
			const PropertyKind kind,
			std::string typeName,
			const std::uint32_t size) {
			auto descriptor = std::make_shared<PropertyDescriptor>();
			descriptor->Kind = kind;
			descriptor->TypeName = std::move(typeName);
			descriptor->Size = size;
			return descriptor;
		};
		const auto makeSupportedProperty = [&makeDescriptor](
			std::string name,
			std::string typeName,
			const PropertyKind kind,
			const std::uint32_t offset,
			const std::uint32_t size) {
			const std::string descriptorTypeName = typeName;
			return ReflectedProperty{
				.Name = std::move(name),
				.TypeName = std::move(typeName),
				.Kind = kind,
				.Offset = offset,
				.Size = size,
				.ArrayDim = 1,
				.Flags = 1,
				.State = ReflectedMemberState::Supported,
				.Descriptor = makeDescriptor(kind, descriptorTypeName, size)
			};
		};
		const auto makeTypeCandidate = [&]() {
			ReflectedProperty inputParameter = makeSupportedProperty(
				"Value", "int32", PropertyKind::Int32, 0, 4);
			inputParameter.Flags = 0x0000000000000080ull;
			ReflectedFunction baseFunction{
				.Handle = {
					.Function = handles[5],
					.Owner = handles[1],
					.FullPath = "Function fname:1:0.fname:5:0",
					.SignatureFingerprint = 0xB001
				},
				.Name = "BaseOnly",
				.FullPath = "/Script/Fixture.Base.BaseOnly",
				.Flags = 1,
				.ParameterSize = 4,
				.NativeAddress = 0x5000,
				.Implementation = ReflectedFunctionImplementation::Native,
				.Parameters = {
					{ReflectedParameterDirection::Input, std::move(inputParameter)}
				}
			};
			ReflectedFunction derivedFunction{
				.Handle = {
					.Function = handles[6],
					.Owner = handles[2],
					.FullPath = "Function fname:2:0.fname:6:0",
					.SignatureFingerprint = 0xD001
				},
				.Name = "DerivedOnly",
				.FullPath = "/Script/Fixture.Derived.DerivedOnly",
				.Flags = 2,
				.ParameterSize = 0,
				.Implementation = ReflectedFunctionImplementation::Bytecode
			};
			return TypeSnapshotCandidate{
				.SessionId = "reflection-facade",
				.ContextGeneration = 51,
				.Generation = 1,
				.ObjectSnapshotGeneration = 7,
				.ReflectionLayoutFingerprint = validated.Layout->Fingerprint(),
				.CapturedAtMonotonicUs = 200,
				.CaptureDurationUs = 20,
				.Source = "synthetic-type-snapshot",
				.Types = {
					{
						.Handle = handles[1],
						.Kind = ReflectedTypeKind::Class,
						.Name = "Base",
						.FullPath = "/Script/Fixture.Base",
						.PackagePath = "/Script/Fixture",
						.PropertiesSize = 16,
						.MinAlignment = 8,
						.DefaultObjectState = ClassDefaultObjectState::Present,
						.DefaultObject = handles[7],
						.DirectProperties = {
							makeSupportedProperty("BaseValue", "int32", PropertyKind::Int32, 0, 4),
							{
								.Name = "OnChanged",
								.TypeName = "delegate",
								.Kind = PropertyKind::Delegate,
								.Offset = 8,
								.Size = 8,
								.ArrayDim = 1,
								.Flags = 1,
								.State = ReflectedMemberState::Unsupported,
								.ReasonCode = "PROPERTY_DELEGATE_UNSUPPORTED",
								.Reason = "Delegate value decoding is not implemented"
							}
						},
						.DirectFunctions = {std::move(baseFunction)}
					},
					{
						.Handle = handles[2],
						.Kind = ReflectedTypeKind::Class,
						.Name = "Derived",
						.FullPath = "/Script/Fixture.Derived",
						.PackagePath = "/Script/Fixture",
						.PropertiesSize = 32,
						.MinAlignment = 8,
						.Super = handles[1],
						.DefaultObjectState = ClassDefaultObjectState::Present,
						.DefaultObject = handles[8],
						.DirectProperties = {
							makeSupportedProperty("DerivedValue", "double", PropertyKind::Double, 16, 8)
						},
						.DirectFunctions = {std::move(derivedFunction)}
					},
					{
						.Handle = handles[3],
						.Kind = ReflectedTypeKind::Struct,
						.Name = "Vector",
						.FullPath = "/Script/Fixture.Vector",
						.PackagePath = "/Script/Fixture",
						.PropertiesSize = 12,
						.MinAlignment = 4,
						.DirectProperties = {
							makeSupportedProperty("X", "float", PropertyKind::Float, 0, 4),
							makeSupportedProperty("Y", "float", PropertyKind::Float, 4, 4),
							makeSupportedProperty("Z", "float", PropertyKind::Float, 8, 4)
						}
					},
					{
						.Handle = handles[4],
						.Kind = ReflectedTypeKind::Enum,
						.Name = "Mode",
						.FullPath = "/Script/Fixture.Mode",
						.PackagePath = "/Script/Fixture",
						.EnumState = ReflectedMemberState::Supported,
						.EnumUnderlyingKind = PropertyKind::Int64,
						.EnumEntries = {
							{"Off", (std::numeric_limits<std::int64_t>::min)()},
							{"On", (std::numeric_limits<std::int64_t>::max)()}
						}
					}
				}
			};
		};

		class SyntheticTypeSnapshotSource final : public ITypeSnapshotSource
		{
		public:
			SyntheticTypeSnapshotSource(
				EngineFacade& engine,
				TypeSnapshotCandidate candidate)
				: m_Engine(engine),
				  m_ContextGeneration(candidate.ContextGeneration),
				  m_Source(std::move(candidate.Source)),
				  m_ExpectedTypes(candidate.Types.size())
			{
				for (ReflectedType& type : candidate.Types)
				{
					std::vector<ReflectedProperty> properties;
					std::vector<ReflectedFunction> functions;
					std::vector<ReflectedEnumEntry> enumEntries;
					properties.swap(type.DirectProperties);
					functions.swap(type.DirectFunctions);
					enumEntries.swap(type.EnumEntries);
					m_Records.emplace_back(TypeSnapshotTypeBegin{std::move(type)});
					for (ReflectedProperty& property : properties)
					{
						m_Records.emplace_back(TypeSnapshotPropertyRecord{
							std::move(property)
						});
					}
					m_ExpectedFunctions += functions.size();
					for (ReflectedFunction& function : functions)
					{
						std::vector<ReflectedParameter> parameters;
						parameters.swap(function.Parameters);
						m_Records.emplace_back(TypeSnapshotFunctionBegin{
							std::move(function)
						});
						for (ReflectedParameter& parameter : parameters)
						{
							m_Records.emplace_back(TypeSnapshotParameterRecord{
								std::move(parameter)
							});
						}
						m_Records.emplace_back(TypeSnapshotFunctionEnd{});
					}
					for (ReflectedEnumEntry& entry : enumEntries)
					{
						m_Records.emplace_back(TypeSnapshotEnumEntryRecord{
							std::move(entry)
						});
					}
					m_Records.emplace_back(TypeSnapshotTypeEnd{});
				}
			}

			std::uint64_t ContextGeneration() const noexcept override
			{
				return m_ContextGeneration;
			}

			bool IsConfigured() const noexcept override
			{
				return m_ContextGeneration != 0 && !m_Source.empty();
			}

			bool IsCurrentExecutionThreadValid() const noexcept override
			{
				return m_ThreadValid;
			}

			TypeSnapshotSourceBeginResult Begin() noexcept override
			{
				m_Cursor = 0;
				m_ValidationCursor = 0;
				m_ValidationBegun = false;
				m_Active = true;
				m_ObjectDependency = m_Engine.Snapshots().Current();
				m_ReflectionDependency = m_Engine.Reflection();
				return {
					.Source = m_Source,
					.ObjectSnapshot = m_ObjectDependency,
					.Reflection = m_ReflectionDependency,
					.ExpectedTypeCount = m_ExpectedTypes,
					.ExpectedFunctionCount = m_ExpectedFunctions
				};
			}

			TypeSnapshotSourceStepResult CaptureNext() noexcept override
			{
				if (!m_Active)
					return {.Error = TypeSnapshotSourceError::ContractViolation};
				if (m_NoProgress)
					return {};
				if (m_Records.empty())
					return {.Progressed = true, .Complete = true};
				if (m_Cursor >= m_Records.size())
					return {.Error = TypeSnapshotSourceError::ContractViolation};
				const std::size_t cursor = m_Cursor++;
				return {
					.Progressed = true,
					.Complete = m_Cursor == m_Records.size(),
					.Record = m_Records[cursor]
				};
			}

			TypeSnapshotSourceError BeginValidation() noexcept override
			{
				if (!m_Active || m_Cursor != m_Records.size())
					return TypeSnapshotSourceError::ContractViolation;
				m_ValidationBegun = true;
				return TypeSnapshotSourceError::None;
			}

			TypeSnapshotSourceValidationResult ValidateNext() noexcept override
			{
				if (!m_Active || !m_ValidationBegun)
					return {.Error = TypeSnapshotSourceError::ContractViolation};
				++m_ValidationCursor;
				return {
					.Progressed = true,
					.Complete = m_ValidationCursor >= m_ValidationSteps
				};
			}

			bool ValidateDependencies() noexcept override
			{
				return m_Active
					&& m_DependenciesValid
					&& m_Engine.Snapshots().Current() == m_ObjectDependency
					&& m_Engine.Reflection() == m_ReflectionDependency;
			}

			void Cancel() noexcept override
			{
				m_Active = false;
				m_ObjectDependency.reset();
				m_ReflectionDependency.reset();
			}

			void SetNoProgress(const bool noProgress) noexcept
			{
				m_NoProgress = noProgress;
			}

			void SetThreadValid(const bool valid) noexcept
			{
				m_ThreadValid = valid;
			}

			void OmitFinalRecord()
			{
				Require(!m_Records.empty(), "Type source fixture had no final record");
				m_Records.pop_back();
			}

			std::size_t RecordCount() const noexcept { return m_Records.size(); }
			std::size_t ValidationStepCount() const noexcept
			{
				return m_ValidationSteps;
			}

		private:
			EngineFacade& m_Engine;
			std::uint64_t m_ContextGeneration = 0;
			std::string m_Source;
			std::vector<TypeSnapshotSourceRecord> m_Records;
			std::shared_ptr<const EngineSnapshot> m_ObjectDependency;
			std::shared_ptr<const ReflectionRuntimeSnapshot> m_ReflectionDependency;
			std::size_t m_ExpectedTypes = 0;
			std::size_t m_ExpectedFunctions = 0;
			std::size_t m_Cursor = 0;
			std::size_t m_ValidationCursor = 0;
			std::size_t m_ValidationSteps = 2;
			bool m_Active = false;
			bool m_ValidationBegun = false;
			bool m_ThreadValid = true;
			bool m_DependenciesValid = true;
			bool m_NoProgress = false;
		};

		const auto publishTypeCaptureDependencies = [&validated, &objectSnapshot](
			EngineFacade& targetFacade) {
			Require(
				targetFacade.ConfigureReflectionLayout(validated.Layout)
					&& targetFacade.Snapshots().Publish(*objectSnapshot).Ok(),
				"Type capture fixture dependencies were not published");
		};
		const auto pumpTypeCapture = [](TypeSnapshotCapture& capture) {
			TypeSnapshotPumpResult result;
			std::size_t work = 0;
			for (std::size_t step = 0; step < 256; ++step)
			{
				result = capture.Pump(1);
				work += result.WorkConsumed;
				if (result.Status == TypeSnapshotPumpStatus::Ready
					|| result.Status == TypeSnapshotPumpStatus::Failed)
				{
					break;
				}
			}
			return std::pair{result, work};
		};

		EngineFacade typeCaptureFacade(context, "reflection-facade", facadeIdentity);
		publishTypeCaptureDependencies(typeCaptureFacade);
		SyntheticTypeSnapshotSource typeCaptureSource(
			typeCaptureFacade,
			makeTypeCandidate());
		Require(
			typeCaptureFacade.ConfigureTypeSnapshotCapture(typeCaptureSource)
				&& !typeCaptureFacade.ConfigureTypeSnapshotCapture(typeCaptureSource),
			"EngineFacade did not uniquely own its type snapshot producer");
		TypeSnapshotCapture* typeCapture = typeCaptureFacade.TypeCapture();
		Require(
			typeCapture
				&& typeCapture->PublishReady().Error == TypeSnapshotPublishError::StoreInvalid
				&& typeCapture->Pump(0).Status == TypeSnapshotPumpStatus::InvalidBudget
				&& typeCapture->RequestCapture() == TypeSnapshotCaptureError::None
				&& typeCapture->RequestCapture() == TypeSnapshotCaptureError::Busy,
			"Type snapshot capture admission or budget validation was not fail-closed");
		const auto [readyPump, captureWork] = pumpTypeCapture(*typeCapture);
		const TypeSnapshotCaptureDiagnostics readyDiagnostics = typeCapture->Diagnostics();
		const std::size_t expectedCaptureWork = 1
			+ typeCaptureSource.RecordCount()
			+ 1
			+ typeCaptureSource.ValidationStepCount()
			+ 1;
		Require(
			readyPump.Status == TypeSnapshotPumpStatus::Ready
				&& captureWork == expectedCaptureWork
				&& readyDiagnostics.State == TypeSnapshotCaptureState::Ready
				&& readyDiagnostics.Error == TypeSnapshotCaptureError::None
				&& readyDiagnostics.CapturedTypes == 4
				&& readyDiagnostics.CapturedFunctions == 2
				&& readyDiagnostics.CapturedMembers == 11
				&& readyDiagnostics.SourceSteps == typeCaptureSource.RecordCount()
				&& readyDiagnostics.ValidationSteps == typeCaptureSource.ValidationStepCount()
				&& !typeCaptureFacade.Types().Current(),
			"Budgeted type capture published partial data or misreported exact work");
		Require(
			typeCapture->PublishReady().Error
					== TypeSnapshotPublishError::WorkerThreadRequired
				&& typeCapture->Diagnostics().State == TypeSnapshotCaptureState::Ready
				&& !typeCaptureFacade.Types().Current(),
			"Type snapshot heavy publication ran on the witnessed game thread");
		const TypeSnapshotPublishResult capturedTypes = std::async(
			std::launch::async,
			[&]() { return typeCapture->PublishReady(); }).get();
		Require(
			capturedTypes.Ok()
				&& capturedTypes.Snapshot == typeCaptureFacade.Types().Current()
				&& capturedTypes.Snapshot->Types().size() == 4
				&& typeCapture->Diagnostics().State == TypeSnapshotCaptureState::Completed
				&& typeCapture->ReclaimRetired() == 0
				&& typeCaptureFacade.Stop(),
			"Worker publication did not atomically publish the complete type generation");

		EngineFacade driftFacade(context, "reflection-facade", facadeIdentity);
		publishTypeCaptureDependencies(driftFacade);
		SyntheticTypeSnapshotSource driftSource(driftFacade, makeTypeCandidate());
		Require(
			driftFacade.ConfigureTypeSnapshotCapture(driftSource)
				&& driftFacade.TypeCapture()->RequestCapture() == TypeSnapshotCaptureError::None,
			"Dependency-drift type capture fixture was not admitted");
		const auto [driftReady, driftWork] = pumpTypeCapture(*driftFacade.TypeCapture());
		EngineSnapshot newerObjects = *objectSnapshot;
		newerObjects.Generation = 8;
		newerObjects.CapturedAtMonotonicUs = 300;
		Require(
			driftReady.Status == TypeSnapshotPumpStatus::Ready
				&& driftWork == expectedCaptureWork
				&& driftFacade.Snapshots().Publish(std::move(newerObjects)).Ok(),
			"Type capture dependency-drift fixture did not reach a sealed candidate");
		const TypeSnapshotPublishResult driftPublication = std::async(
			std::launch::async,
			[&]() { return driftFacade.TypeCapture()->PublishReady(); }).get();
		const TypeSnapshotCaptureDiagnostics driftDiagnostics =
			driftFacade.TypeCapture()->Diagnostics();
		Require(
			driftPublication.Error == TypeSnapshotPublishError::DependencyInvalid
				&& driftDiagnostics.State == TypeSnapshotCaptureState::Failed
				&& driftDiagnostics.Error == TypeSnapshotCaptureError::DependencyChanged
				&& driftDiagnostics.SourceError == TypeSnapshotSourceError::DependencyChanged
				&& driftDiagnostics.PublishError == TypeSnapshotPublishError::DependencyInvalid
				&& !driftFacade.Types().Current()
				&& std::async(
					std::launch::async,
					[&]() { return driftFacade.TypeCapture()->ReclaimRetired(); }).get() == 1
				&& driftFacade.Stop(),
			"A changed immutable dependency reached type snapshot publication");

		EngineFacade threadFacade(context, "reflection-facade", facadeIdentity);
		publishTypeCaptureDependencies(threadFacade);
		SyntheticTypeSnapshotSource threadSource(threadFacade, makeTypeCandidate());
		Require(
			threadFacade.ConfigureTypeSnapshotCapture(threadSource)
				&& threadFacade.TypeCapture()->RequestCapture()
					== TypeSnapshotCaptureError::None
				&& threadFacade.TypeCapture()->Pump(1 + threadSource.RecordCount()).Status
					== TypeSnapshotPumpStatus::Progress
				&& threadFacade.TypeCapture()->Diagnostics().State
					== TypeSnapshotCaptureState::Validating,
			"Type execution-thread fixture did not reach incremental validation");
		threadSource.SetThreadValid(false);
		Require(
			threadFacade.TypeCapture()->Pump(1).Status == TypeSnapshotPumpStatus::Failed
				&& threadFacade.TypeCapture()->Diagnostics().Error
					== TypeSnapshotCaptureError::ExecutionThreadInvalid
				&& !threadFacade.Types().Current()
				&& threadFacade.TypeCapture()->ReclaimRetired() == 1
				&& threadFacade.Stop(),
			"Type validation continued after execution-thread invalidation");

		EngineFacade stalledFacade(context, "reflection-facade", facadeIdentity);
		publishTypeCaptureDependencies(stalledFacade);
		SyntheticTypeSnapshotSource stalledSource(stalledFacade, makeTypeCandidate());
		stalledSource.SetNoProgress(true);
		Require(
			stalledFacade.ConfigureTypeSnapshotCapture(stalledSource),
			"No-progress type source fixture was not configured");
		for (std::size_t failure = 0;
			failure < TypeSnapshotCapture::kMaxRetiredCandidates;
			++failure)
		{
			Require(
				stalledFacade.TypeCapture()->RequestCapture()
						== TypeSnapshotCaptureError::None
					&& stalledFacade.TypeCapture()->Pump(2).Status
						== TypeSnapshotPumpStatus::Failed
					&& stalledFacade.TypeCapture()->Diagnostics().Error
						== TypeSnapshotCaptureError::SourceContractViolation
					&& stalledFacade.TypeCapture()->Diagnostics().RetiredCandidates
						== failure + 1,
				"A no-progress type source escaped bounded retirement");
		}
		Require(
			stalledFacade.TypeCapture()->RequestCapture() == TypeSnapshotCaptureError::None
				&& stalledFacade.TypeCapture()->Pump(2).Status
					== TypeSnapshotPumpStatus::Failed
				&& stalledFacade.TypeCapture()->Diagnostics().Error
					== TypeSnapshotCaptureError::RetirementBackpressure
				&& stalledFacade.TypeCapture()->RequestCapture()
					== TypeSnapshotCaptureError::RetirementBackpressure
				&& std::async(
					std::launch::async,
					[&]() { return stalledFacade.TypeCapture()->ReclaimRetired(); }).get()
					== TypeSnapshotCapture::kMaxRetiredCandidates + 1
				&& stalledFacade.TypeCapture()->RequestCapture()
					== TypeSnapshotCaptureError::None
				&& stalledFacade.Stop(),
			"Type capture retirement backpressure lost or frame-thread-destroyed a candidate");

		EngineFacade malformedFacade(context, "reflection-facade", facadeIdentity);
		publishTypeCaptureDependencies(malformedFacade);
		SyntheticTypeSnapshotSource malformedSource(malformedFacade, makeTypeCandidate());
		malformedSource.OmitFinalRecord();
		Require(
			malformedFacade.ConfigureTypeSnapshotCapture(malformedSource)
				&& malformedFacade.TypeCapture()->RequestCapture()
					== TypeSnapshotCaptureError::None
				&& malformedFacade.TypeCapture()->Pump(TypeSnapshotCapture::kMaxPumpBudget).Status
					== TypeSnapshotPumpStatus::Failed
				&& malformedFacade.TypeCapture()->Diagnostics().Error
					== TypeSnapshotCaptureError::SourceContractViolation
				&& malformedFacade.TypeCapture()->Diagnostics().SourceError
					== TypeSnapshotSourceError::ContractViolation
				&& !malformedFacade.Types().Current()
				&& malformedFacade.TypeCapture()->ReclaimRetired() == 1
				&& malformedFacade.Stop(),
			"An incomplete type/function record stream reached immutable publication");

		TypeSnapshotStore validationStore("reflection-facade", 51);
		const auto publishTypeCandidate = [&facade, &validationStore](
			TypeSnapshotCandidate candidate) {
			return validationStore.Publish(
				std::move(candidate),
				facade.Snapshots().Current(),
				facade.Reflection());
		};
		TypeSnapshotCandidate partialTypes = makeTypeCandidate();
		partialTypes.Types.pop_back();
		Require(
			publishTypeCandidate(std::move(partialTypes)).Error
				== TypeSnapshotPublishError::TypeCoverageMismatch,
			"A partial type snapshot was published");
		TypeSnapshotCandidate missingFunction = makeTypeCandidate();
		missingFunction.Types[0].DirectFunctions.clear();
		Require(
			publishTypeCandidate(std::move(missingFunction)).Error
				== TypeSnapshotPublishError::FunctionCoverageMismatch,
			"A type snapshot omitted a live direct function");
		TypeSnapshotCandidate wrongDefaultObject = makeTypeCandidate();
		wrongDefaultObject.Types[1].DefaultObject = handles[7];
		Require(
			publishTypeCandidate(std::move(wrongDefaultObject)).Error
				== TypeSnapshotPublishError::RelationshipInvalid,
			"A class accepted another class's default object");
		TypeSnapshotCandidate cyclicHierarchy = makeTypeCandidate();
		cyclicHierarchy.Types[0].PropertiesSize = 32;
		cyclicHierarchy.Types[0].Super = handles[2];
		Require(
			publishTypeCandidate(std::move(cyclicHierarchy)).Error
				== TypeSnapshotPublishError::HierarchyCycle,
			"A cyclic class hierarchy was published");
		TypeSnapshotCandidate recursiveDescriptor = makeTypeCandidate();
		auto recursive = std::make_shared<PropertyDescriptor>();
		recursive->Kind = PropertyKind::Struct;
		recursive->TypeName = "Recursive";
		recursive->Size = 8;
		recursive->Fields.push_back({"Self", 0, recursive});
		ReflectedProperty& recursiveProperty = recursiveDescriptor.Types[0].DirectProperties[0];
		recursiveProperty.TypeName = "Recursive";
		recursiveProperty.Kind = PropertyKind::Struct;
		recursiveProperty.Size = 8;
		recursiveProperty.Descriptor = recursive;
		Require(
			publishTypeCandidate(std::move(recursiveDescriptor)).Error
				== TypeSnapshotPublishError::DescriptorCycle,
			"A cyclic mutable property descriptor was frozen into a type snapshot");
		TypeSnapshotCandidate invalidMemberState = makeTypeCandidate();
		invalidMemberState.Types[0].DirectProperties[0].State =
			static_cast<ReflectedMemberState>(0xFF);
		Require(
			publishTypeCandidate(std::move(invalidMemberState)).Error
				== TypeSnapshotPublishError::PropertyInvalid,
			"An unknown reflected member state was published");
		TypeSnapshotCandidate wrongParameterDirection = makeTypeCandidate();
		wrongParameterDirection.Types[0].DirectFunctions[0].Parameters[0].Direction =
			ReflectedParameterDirection::Output;
		Require(
			publishTypeCandidate(std::move(wrongParameterDirection)).Error
				== TypeSnapshotPublishError::FunctionInvalid,
			"Parameter direction disagreed with its reflected flags");

		TypeSnapshotCandidate validTypes = makeTypeCandidate();
		auto mutableDescriptor = std::const_pointer_cast<PropertyDescriptor>(
			validTypes.Types[0].DirectProperties[0].Descriptor);
		const TypeSnapshotPublishResult typePublished = publishTypeCandidate(
			std::move(validTypes));
		Require(
			typePublished.Ok()
				&& typePublished.Snapshot->IsConfigured(51)
				&& typePublished.Snapshot->Types().size() == 4,
			"A complete generation-bound type snapshot was rejected");
		mutableDescriptor->TypeName = "corrupted-after-publication";
		mutableDescriptor->Size = 1;
		const ReflectedType* frozenBase = typePublished.Snapshot->FindByFullPath(
			"/Script/Fixture.Base");
		const ReflectedType* frozenDerived = typePublished.Snapshot->FindByObjectIndex(2);
		Require(
			frozenBase
				&& frozenDerived
				&& frozenBase->DirectProperties[0].Descriptor
				&& frozenBase->DirectProperties[0].Descriptor->TypeName == "int32"
				&& frozenBase->DirectProperties[0].Descriptor->Size == 4
				&& frozenDerived->DefaultObject
				&& frozenDerived->DefaultObject->Index == 8,
			"Published type metadata retained a mutable descriptor alias or guessed its CDO");

		const TypePropertyQueryResult directProperties = QueryTypeProperties(
			typePublished.Snapshot,
			"/Script/Fixture.Derived",
			TypeMemberScope::Direct);
		const TypePropertyQueryResult inheritedProperties = QueryTypeProperties(
			typePublished.Snapshot,
			"/Script/Fixture.Derived",
			TypeMemberScope::IncludeInherited);
		const TypeFunctionQueryResult inheritedFunctions = QueryTypeFunctions(
			typePublished.Snapshot,
			"/Script/Fixture.Derived",
			TypeMemberScope::IncludeInherited);
		const TypePropertyQueryResult invalidScope = QueryTypeProperties(
			typePublished.Snapshot,
			"/Script/Fixture.Derived",
			static_cast<TypeMemberScope>(0xFF));
		Require(
			directProperties.Ok()
				&& directProperties.Members.size() == 1
				&& directProperties.Members[0].Member->Name == "DerivedValue"
				&& directProperties.Members[0].InheritanceDepth == 0
				&& inheritedProperties.Ok()
				&& inheritedProperties.Members.size() == 3
				&& inheritedProperties.Members[0].Member->Name == "DerivedValue"
				&& inheritedProperties.Members[1].Member->Name == "BaseValue"
				&& inheritedProperties.Members[1].InheritanceDepth == 1
				&& inheritedProperties.Members[2].Member->State == ReflectedMemberState::Unsupported
				&& inheritedProperties.Members[2].Member->ReasonCode == "PROPERTY_DELEGATE_UNSUPPORTED"
				&& inheritedFunctions.Ok()
				&& inheritedFunctions.Members.size() == 2
				&& inheritedFunctions.Members[0].Member->Name == "DerivedOnly"
				&& inheritedFunctions.Members[1].Member->Name == "BaseOnly"
				&& invalidScope.Error == TypeMemberQueryError::ScopeInvalid,
			"Direct and inherited type-member semantics were not explicit and stable");

		const auto executeType = [&typePublished](
			const std::string_view operation,
			const nlohmann::json& data) {
			return UExplorer::Services::TypeCommandService::Execute(
				typePublished.Snapshot,
				operation,
				data,
				"reflection-facade",
				51,
				7);
		};
		Require(
			UExplorer::Services::TypeCommandService::Handles("types.classes.get")
				&& UExplorer::Services::TypeCommandService::Handles("types.functions.get")
				&& UExplorer::Services::TypeCommandService::Handles("types.structs.fields")
				&& UExplorer::Services::TypeCommandService::Handles("types.enums.values")
				&& !UExplorer::Services::TypeCommandService::Handles("types.classes.legacy"),
			"Immutable type command registry is incomplete or accepts a legacy route");

		const auto classDetail = executeType(
			"types.classes.get", {{"path", "/Script/Fixture.Derived"}});
		Require(
			classDetail.Ok()
				&& classDetail.Data.at("type_snapshot_generation") == 1
				&& classDetail.Data.at("object_snapshot_generation") == 7
				&& classDetail.Data.at("kind") == "class"
				&& classDetail.Data.at("properties_size") == 32
				&& classDetail.Data.at("super").at("full_path") == "/Script/Fixture.Base"
				&& classDetail.Data.at("default_object").at("state") == "present"
				&& classDetail.Data.at("default_object").at("handle").at("index") == 8
				&& !classDetail.Data.contains("fields")
				&& !classDetail.Data.contains("functions"),
			"Class detail did not preserve exact hierarchy/CDO metadata or returned an unbounded member set");
		const auto legacyClassInput = executeType(
			"types.classes.get", {{"name", "Derived"}});
		Require(
			!legacyClassInput.Ok()
				&& legacyClassInput.Error->Code == "INVALID_ARGUMENT",
			"Type detail accepted a short-name legacy identity");

		const auto firstFields = executeType(
			"types.classes.fields",
			{
				{"path", "/Script/Fixture.Derived"},
				{"scope", "include_inherited"},
				{"cursor", nullptr},
				{"limit", 2}
			});
		Require(
			firstFields.Ok()
				&& firstFields.Data.at("total") == 3
				&& firstFields.Data.at("items").size() == 2
				&& firstFields.Data.at("items")[0].at("name") == "DerivedValue"
				&& firstFields.Data.at("items")[0].at("inheritance_depth") == 0
				&& firstFields.Data.at("items")[1].at("name") == "BaseValue"
				&& firstFields.Data.at("items")[1].at("inheritance_depth") == 1
				&& firstFields.Data.at("items")[1].at("flags") == "0x0000000000000001"
				&& firstFields.Data.at("has_more").get<bool>()
				&& firstFields.Data.at("next_cursor").at("after_ordinal") == 1,
			"Inherited class field paging lost ordering, flags, or exact totals");
		const auto finalFields = executeType(
			"types.classes.fields",
			{
				{"path", "/Script/Fixture.Derived"},
				{"scope", "include_inherited"},
				{"cursor", firstFields.Data.at("next_cursor")},
				{"limit", 2}
			});
		Require(
			finalFields.Ok()
				&& finalFields.Data.at("items").size() == 1
				&& finalFields.Data.at("items")[0].at("name") == "OnChanged"
				&& finalFields.Data.at("items")[0].at("state") == "unsupported"
				&& finalFields.Data.at("items")[0].at("reason_code")
					== "PROPERTY_DELEGATE_UNSUPPORTED"
				&& !finalFields.Data.at("has_more").get<bool>()
				&& finalFields.Data.at("next_cursor").is_null(),
			"Unsupported field state was erased or the final type page did not terminate");
		nlohmann::json exhaustedCursor = firstFields.Data.at("next_cursor");
		exhaustedCursor["after_ordinal"] = 2;
		const auto exhaustedFields = executeType(
			"types.classes.fields",
			{
				{"path", "/Script/Fixture.Derived"},
				{"scope", "include_inherited"},
				{"cursor", exhaustedCursor},
				{"limit", 2}
			});
		Require(
			!exhaustedFields.Ok()
				&& exhaustedFields.Error->Code == "TYPE_QUERY_CURSOR_INVALID",
			"Type paging accepted a fabricated cursor that cannot have a continuation");

		nlohmann::json wrongQueryCursor = firstFields.Data.at("next_cursor");
		const auto wrongQuery = executeType(
			"types.classes.fields",
			{
				{"path", "/Script/Fixture.Base"},
				{"scope", "include_inherited"},
				{"cursor", wrongQueryCursor},
				{"limit", 1}
			});
		wrongQueryCursor["generation"] = 2;
		const auto wrongGeneration = executeType(
			"types.classes.fields",
			{
				{"path", "/Script/Fixture.Derived"},
				{"scope", "include_inherited"},
				{"cursor", wrongQueryCursor},
				{"limit", 1}
			});
		Require(
			!wrongQuery.Ok()
				&& wrongQuery.Error->Code == "TYPE_QUERY_CURSOR_MISMATCH"
				&& !wrongGeneration.Ok()
				&& wrongGeneration.Error->Code == "TYPE_SNAPSHOT_GENERATION_MISMATCH",
			"Type paging cursor crossed a query or immutable generation boundary");

		const auto firstFunctions = executeType(
			"types.classes.functions",
			{
				{"path", "/Script/Fixture.Derived"},
				{"scope", "include_inherited"},
				{"cursor", nullptr},
				{"limit", 1}
			});
		const auto finalFunctions = executeType(
			"types.classes.functions",
			{
				{"path", "/Script/Fixture.Derived"},
				{"scope", "include_inherited"},
				{"cursor", firstFunctions.Data.at("next_cursor")},
				{"limit", 1}
			});
		const auto exactFunction = executeType(
			"types.functions.get",
			{{"path", "/Script/Fixture.Base.BaseOnly"}});
		Require(
			firstFunctions.Ok()
				&& firstFunctions.Data.at("items")[0].at("name") == "DerivedOnly"
				&& firstFunctions.Data.at("items")[0].at("implementation") == "bytecode"
				&& firstFunctions.Data.at("items")[0].at("native_address").is_null()
				&& finalFunctions.Ok()
				&& finalFunctions.Data.at("items")[0].at("name") == "BaseOnly"
				&& finalFunctions.Data.at("items")[0].at("implementation") == "native"
				&& finalFunctions.Data.at("items")[0].at("native_address") == "0x5000"
				&& finalFunctions.Data.at("items")[0].at("parameters")[0].at("direction") == "input"
				&& finalFunctions.Data.at("items")[0].at("parameters")[0].at("type_name") == "int32"
				&& exactFunction.Ok()
				&& exactFunction.Data.at("full_path")
					== "/Script/Fixture.Base.BaseOnly"
				&& exactFunction.Data.at("declaring_type").at("full_path")
					== "/Script/Fixture.Base",
			"Function pages guessed implementation state or lost parameter metadata");

		const auto hierarchy = executeType(
			"types.classes.hierarchy",
			{
				{"path", "/Script/Fixture.Base"},
				{"cursor", nullptr},
				{"limit", 1}
			});
		Require(
			hierarchy.Ok()
				&& hierarchy.Data.at("parents").empty()
				&& hierarchy.Data.at("children").size() == 1
				&& hierarchy.Data.at("children")[0].at("full_path")
					== "/Script/Fixture.Derived"
				&& hierarchy.Data.at("total") == 1,
			"Class hierarchy did not use the immutable direct-child index");

		const auto classCdo = executeType(
			"types.classes.cdo", {{"path", "/Script/Fixture.Derived"}});
		const auto structDetail = executeType(
			"types.structs.get", {{"path", "/Script/Fixture.Vector"}});
		const auto structFields = executeType(
			"types.structs.fields",
			{
				{"path", "/Script/Fixture.Vector"},
				{"scope", "direct"},
				{"cursor", nullptr},
				{"limit", 2}
			});
		Require(
			classCdo.Ok()
				&& classCdo.Data.at("state") == "present"
				&& classCdo.Data.at("handle").at("index") == 8
				&& !classCdo.Data.contains("properties")
				&& structDetail.Ok()
				&& structDetail.Data.at("properties_size") == 12
				&& structFields.Ok()
				&& structFields.Data.at("total") == 3
				&& structFields.Data.at("items")[0].at("name") == "X"
				&& structFields.Data.at("has_more").get<bool>(),
			"Class CDO or struct metadata returned fabricated values or unbounded fields");

		const auto enumDetail = executeType(
			"types.enums.get", {{"path", "/Script/Fixture.Mode"}});
		const auto firstEnumValues = executeType(
			"types.enums.values",
			{
				{"path", "/Script/Fixture.Mode"},
				{"cursor", nullptr},
				{"limit", 1}
			});
		const auto finalEnumValues = executeType(
			"types.enums.values",
			{
				{"path", "/Script/Fixture.Mode"},
				{"cursor", firstEnumValues.Data.at("next_cursor")},
				{"limit", 1}
			});
		Require(
			enumDetail.Ok()
				&& enumDetail.Data.at("enum").at("underlying_kind") == "int64"
				&& enumDetail.Data.at("enum").at("value_count") == 2
				&& !enumDetail.Data.contains("values")
				&& firstEnumValues.Ok()
				&& firstEnumValues.Data.at("items")[0].at("value")
					== "-9223372036854775808"
				&& finalEnumValues.Ok()
				&& finalEnumValues.Data.at("items")[0].at("value")
					== "9223372036854775807",
			"Enum query truncated int64 values or embedded an unbounded value array");

		const auto wrongKind = executeType(
			"types.classes.get", {{"path", "/Script/Fixture.Vector"}});
		const auto wrongContext = UExplorer::Services::TypeCommandService::Execute(
			typePublished.Snapshot,
			"types.classes.get",
			{{"path", "/Script/Fixture.Derived"}},
			"reflection-facade",
			52,
			7);
		Require(
			!wrongKind.Ok()
				&& wrongKind.Error->Code == "TYPE_KIND_INVALID"
				&& !wrongContext.Ok()
				&& wrongContext.Error->Code == "TYPE_SNAPSHOT_CONTEXT_MISMATCH",
			"Type commands crossed a kind or immutable context-generation boundary");

		TypeSnapshotCandidate repeatedGeneration = makeTypeCandidate();
		Require(
			publishTypeCandidate(std::move(repeatedGeneration)).Error
				== TypeSnapshotPublishError::GenerationNotMonotonic,
			"A non-increasing type snapshot generation replaced the current snapshot");
		RuntimeProbes typeProbes;
		typeProbes.GameThreadExecutorEnabled = true;
		typeProbes.GameThreadPumpObserved = true;
		typeProbes.GameThreadPumpThreadStable = true;
		typeProbes.GameThreadPumpActive = true;
		typeProbes.SafeMemoryEnabled = true;
		typeProbes.ObjectIdentitySourceEnabled = true;
		typeProbes.ObjectHandleValidationEnabled = true;
		typeProbes.ObjectSnapshotPublished = true;
		typeProbes.ObjectSnapshotGeneration = 7;
		typeProbes.Reflection = retained;
		typeProbes.Types = typePublished.Snapshot;
		const auto typeCapabilities = BuildCoreCapabilities(*context, typeProbes);
		Require(
			typeCapabilities->IsAvailable("engine.type_snapshot")
				&& typeCapabilities->IsAvailable("types.inspect"),
			"Type command capability did not follow the complete immutable type snapshot dependency");
		typeProbes.ObjectSnapshotGeneration = 8;
		const auto staleTypeCapabilities = BuildCoreCapabilities(*context, typeProbes);
		Require(
			!staleTypeCapabilities->IsAvailable("engine.type_snapshot"),
			"A type snapshot survived an object snapshot generation change");

		std::int32_t integer = 42;
		PropertyDescriptor integerDescriptor{
			.Kind = PropertyKind::Int32,
			.TypeName = "int32",
			.Size = sizeof(integer)
		};
		Require(
			facade.Stop()
				&& !facade.Reflection()
				&& !facade.Properties()
				&& !facade.Types().Current()
				&& validationStore.Current() == typePublished.Snapshot
				&& typePublished.Snapshot->IsConfigured(51)
				&& retained->IsConfigured(51)
				&& retainedCodec->Decode(
					reinterpret_cast<std::uintptr_t>(&integer),
					integerDescriptor).Ok(),
			"EngineFacade did not atomically unpublish a self-contained reflection snapshot");
	}

	void TestCoreRuntimeStateAndShutdown()
	{
		using namespace UExplorer::Runtime;

		CoreRuntime runtime;
		Require(
			runtime.BeginInitialize("fixture-core-session"),
			"CoreRuntime rejected Created -> Initializing");
		Require(
			runtime.Snapshot().SessionId == "fixture-core-session",
			"CoreRuntime did not own its session identity");
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
		probes.FunctionHandleValidationEnabled = true;
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

	class FakeHandleIdentitySource final : public UExplorer::Runtime::IObjectSnapshotIdentitySource
	{
	public:
		bool Available = true;
		bool ThrowOnRead = false;
		bool ExecutionThreadValid = true;
		std::uint64_t Generation = 42;
		std::int32_t ObjectCount = 0;
		std::unordered_map<std::int32_t, UExplorer::Runtime::ObjectIdentity> Objects;
		std::unordered_map<std::int32_t, UExplorer::Runtime::FunctionIdentity> Functions;

		std::uint64_t ContextGeneration() const noexcept override { return Generation; }
		bool IsCurrentExecutionThreadValid() const noexcept override
		{
			return ExecutionThreadValid;
		}

		bool CanReadObjectSlots() const noexcept override
		{
			return Available;
		}

		bool TryGetObjectCount(std::int32_t& objectCount) override
		{
			if (ThrowOnRead)
				throw std::runtime_error("fixture identity source failure");
			if (!Available || !ExecutionThreadValid || ObjectCount < 0)
				return false;
			objectCount = ObjectCount;
			return true;
		}

		UExplorer::Runtime::ObjectSnapshotSlotReadResult TryReadObjectSlot(
			const std::int32_t index,
			UExplorer::Runtime::ObjectIdentity& identity) override
		{
			if (ThrowOnRead)
				throw std::runtime_error("fixture identity source failure");
			identity = {};
			if (!Available || !ExecutionThreadValid || index < 0 || index >= ObjectCount)
				return UExplorer::Runtime::ObjectSnapshotSlotReadResult::Failed;
			const auto found = Objects.find(index);
			if (found == Objects.end())
				return UExplorer::Runtime::ObjectSnapshotSlotReadResult::Empty;
			identity = found->second;
			return UExplorer::Runtime::ObjectSnapshotSlotReadResult::Captured;
		}

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

	void TestObjectSnapshotReflectionCandidateSource()
	{
		using namespace UExplorer::Runtime;

		constexpr std::uint64_t contextGeneration = 77;
		constexpr std::string_view sessionId = "fixture-reflection-source";
		constexpr std::int32_t structSuperOffset = 0x40;
		constexpr std::int32_t structChildrenOffset = 0x50;
		constexpr std::int32_t structPropertiesSizeOffset = 0x58;
		constexpr std::int32_t structMinAlignmentOffset = 0x5C;
		constexpr std::int32_t structContainerSize = 0xA0;
		constexpr std::int32_t fieldNextOffset = 0x28;
		constexpr std::int32_t propertyArrayDimOffset = 0x30;
		constexpr std::int32_t propertyElementSizeOffset = 0x34;
		constexpr std::int32_t propertyFlagsOffset = 0x38;
		constexpr std::int32_t propertyOffsetOffset = 0x40;
		constexpr std::int32_t derivedPropertyOffset = 0x80;

		constexpr std::uint64_t castField = 0x0000000000000001;
		constexpr std::uint64_t castByte = 0x0000000000000040;
		constexpr std::uint64_t castInt = 0x0000000000000080;
		constexpr std::uint64_t castName = 0x0000000000002000;
		constexpr std::uint64_t castProperty = 0x0000000000008000;
		constexpr std::uint64_t castObject = 0x0000000000010000;
		constexpr std::uint64_t castBool = 0x0000000000020000;
		constexpr std::uint64_t castFunction = 0x0000000000080000;
		constexpr std::uint64_t castStruct = 0x0000000000100000;
		constexpr std::uint64_t castArray = 0x0000000000200000;
		constexpr std::uint64_t castNumeric = 0x0000000001000000;
		constexpr std::uint64_t castText = 0x0000000040000000;
		constexpr std::uint64_t castMap = 0x0000400000000000;
		constexpr std::uint64_t castSet = 0x0000800000000000;
		constexpr std::uint64_t castEnum = 0x0001000000000000;
		(void)castField;

		struct alignas(16) FixtureBlock final
		{
			std::array<std::byte, 0x240> Bytes{};

			std::uintptr_t Address() noexcept
			{
				return reinterpret_cast<std::uintptr_t>(Bytes.data());
			}
		};
		struct FixtureNode final
		{
			FixtureBlock* Memory = nullptr;
			std::int32_t Index = -1;
		};

		std::vector<std::unique_ptr<FixtureBlock>> memory;
		const auto allocateBlock = [&]() -> FixtureBlock& {
			memory.push_back(std::make_unique<FixtureBlock>());
			return *memory.back();
		};
		const auto write = []<typename T>(
			FixtureBlock& block,
			const std::size_t offset,
			const T& value) {
			static_assert(std::is_trivially_copyable_v<T>);
			Require(
				offset <= block.Bytes.size()
					&& sizeof(T) <= block.Bytes.size() - offset,
				"Reflection fixture write exceeded its backing block");
			std::memcpy(block.Bytes.data() + offset, &value, sizeof(T));
		};

		FakeHandleIdentitySource identitySource;
		identitySource.Generation = contextGeneration;
		std::int32_t nextIndex = 1;
		EngineSnapshot snapshot{
			.SessionId = std::string(sessionId),
			.ContextGeneration = contextGeneration,
			.Generation = 1,
			.CapturedAtMonotonicUs = 1000,
			.CaptureDurationUs = 10
		};
		std::unordered_map<std::string, FixtureNode> nodes;

		const auto addIdentity = [&](FixtureBlock& block) {
			const std::int32_t index = nextIndex++;
			const ObjectIdentity identity{
				.Index = index,
				.SerialNumber = 1000 + index,
				.Address = block.Address(),
				.ClassFingerprint = 0xA000u + static_cast<std::uint64_t>(index)
			};
			identitySource.Objects.emplace(index, identity);
			return identity;
		};
		const auto addSnapshotObject = [&](const std::string_view path,
			const EngineObjectKind kind) -> FixtureBlock& {
			FixtureBlock& block = allocateBlock();
			const ObjectIdentity identity = addIdentity(block);
			const std::string fullPath(path);
			const std::size_t nameStart = fullPath.find_last_of('.');
			const std::size_t packageEnd = fullPath.find('.');
			snapshot.Objects.push_back({
				.Handle = {
					.SessionId = std::string(sessionId),
					.ContextGeneration = contextGeneration,
					.Index = identity.Index,
					.SerialNumber = identity.SerialNumber,
					.Address = identity.Address,
					.ClassFingerprint = identity.ClassFingerprint
				},
				.Name = nameStart == std::string::npos
					? fullPath
					: fullPath.substr(nameStart + 1),
				.FullPath = fullPath,
				.ClassPath = "/Script/CoreUObject.Class",
				.PackagePath = packageEnd == std::string::npos
					? fullPath
					: fullPath.substr(0, packageEnd),
				.Kind = kind
			});
			nodes.emplace(fullPath, FixtureNode{&block, identity.Index});
			return block;
		};
		const auto addPropertyClass = [&](const std::uint64_t castFlags)
			-> FixtureBlock& {
			FixtureBlock& block = allocateBlock();
			write(block, 0x38, castFlags);
			return block;
		};
		const auto initializeProperty = [&](FixtureBlock& property,
			const std::int32_t index,
			FixtureBlock& propertyClass) {
			write(property, 0x0C, index);
			write(property, 0x10, propertyClass.Address());
		};

		struct RequiredObject final
		{
			std::string_view Path;
			EngineObjectKind Kind;
		};
		constexpr std::array requiredObjects{
			RequiredObject{"/Script/CoreUObject.Struct", EngineObjectKind::Class},
			RequiredObject{"/Script/CoreUObject.Field", EngineObjectKind::Class},
			RequiredObject{"/Script/CoreUObject.Class", EngineObjectKind::Class},
			RequiredObject{"/Script/CoreUObject.Guid", EngineObjectKind::Struct},
			RequiredObject{"/Script/CoreUObject.Color", EngineObjectKind::Struct},
			RequiredObject{"/Script/CoreUObject.Vector", EngineObjectKind::Struct},
			RequiredObject{"/Script/CoreUObject.TwoVectors", EngineObjectKind::Struct},
			RequiredObject{"/Script/Engine.Engine", EngineObjectKind::Class},
			RequiredObject{"/Script/Engine.PlayerController", EngineObjectKind::Class},
			RequiredObject{"/Script/Engine.LocalPlayer", EngineObjectKind::Class},
			RequiredObject{"/Script/Engine.CollisionResponseContainer", EngineObjectKind::Struct},
			RequiredObject{"/Script/Engine.Controller", EngineObjectKind::Class},
			RequiredObject{"/Script/Engine.PlayerState", EngineObjectKind::Class},
			RequiredObject{"/Script/Engine.Pawn", EngineObjectKind::Class},
			RequiredObject{"/Script/Engine.GameViewportClient", EngineObjectKind::Class},
			RequiredObject{"/Script/Engine.UserDefinedEnum", EngineObjectKind::Class},
			RequiredObject{"/Script/Engine.LevelCollection", EngineObjectKind::Struct},
			RequiredObject{"/Script/Engine.ActorComponent", EngineObjectKind::Class},
			RequiredObject{"/Script/Engine.DebugDisplayProperty", EngineObjectKind::Struct},
			RequiredObject{"/Script/Engine.Level", EngineObjectKind::Class},
			RequiredObject{"/Script/Engine.ECollisionResponse", EngineObjectKind::Enum},
			RequiredObject{"/Script/Engine.EComponentCreationMethod", EngineObjectKind::Enum},
			RequiredObject{"/Script/Engine.EAutoPossessAI", EngineObjectKind::Enum}
		};
		for (const RequiredObject& required : requiredObjects)
			(void)addSnapshotObject(required.Path, required.Kind);

		FixtureBlock& intClass = addPropertyClass(
			castProperty | castNumeric | castInt);
		FixtureBlock& byteClass = addPropertyClass(
			castProperty | castNumeric | castByte);
		FixtureBlock& boolClass = addPropertyClass(castProperty | castBool);
		FixtureBlock& objectClass = addPropertyClass(castProperty | castObject);
		FixtureBlock& structClass = addPropertyClass(castProperty | castStruct);
		FixtureBlock& arrayClass = addPropertyClass(castProperty | castArray);
		FixtureBlock& mapClass = addPropertyClass(castProperty | castMap);
		FixtureBlock& setClass = addPropertyClass(castProperty | castSet);
		FixtureBlock& enumClass = addPropertyClass(castProperty | castEnum);
		FixtureBlock& nameClass = addPropertyClass(castProperty | castName);
		FixtureBlock& textClass = addPropertyClass(castProperty | castText);

		const auto addSnapshotProperty = [&](const std::string_view path,
			FixtureBlock& propertyClass) -> FixtureBlock& {
			FixtureBlock& property = addSnapshotObject(path, EngineObjectKind::Object);
			initializeProperty(property, snapshot.Objects.back().Handle.Index, propertyClass);
			return property;
		};
		FixtureBlock& guidA = addSnapshotProperty("/Script/CoreUObject.Guid.A", intClass);
		FixtureBlock& guidC = addSnapshotProperty("/Script/CoreUObject.Guid.C", intClass);
		FixtureBlock& guidD = addSnapshotProperty("/Script/CoreUObject.Guid.D", intClass);
		FixtureBlock& colorR = addSnapshotProperty("/Script/CoreUObject.Color.R", byteClass);
		FixtureBlock& colorB = addSnapshotProperty("/Script/CoreUObject.Color.B", byteClass);
		FixtureBlock& colorG = addSnapshotProperty("/Script/CoreUObject.Color.G", byteClass);
		FixtureBlock& engineBool = addSnapshotProperty(
			"/Script/Engine.Engine.bIsOverridingSelectedColor", boolClass);
		FixtureBlock& controllerBool = addSnapshotProperty(
			"/Script/Engine.PlayerController.bAutoManageActiveCameraTarget", boolClass);
		FixtureBlock& traceChannel1 = addSnapshotProperty(
			"/Script/Engine.CollisionResponseContainer.GameTraceChannel1", byteClass);
		FixtureBlock& traceChannel2 = addSnapshotProperty(
			"/Script/Engine.CollisionResponseContainer.GameTraceChannel2", byteClass);
		FixtureBlock& controllerPlayerState = addSnapshotProperty(
			"/Script/Engine.Controller.PlayerState", objectClass);
		FixtureBlock& controllerPawn = addSnapshotProperty(
			"/Script/Engine.Controller.Pawn", objectClass);
		FixtureBlock& twoVectorsV1 = addSnapshotProperty(
			"/Script/CoreUObject.TwoVectors.v1", structClass);
		FixtureBlock& twoVectorsV2 = addSnapshotProperty(
			"/Script/CoreUObject.TwoVectors.v2", structClass);
		FixtureBlock& debugProperties = addSnapshotProperty(
			"/Script/Engine.GameViewportClient.DebugProperties", arrayClass);
		FixtureBlock& localPlayers = addSnapshotProperty(
			"/Script/Engine.GameViewportClient.LocalPlayers", arrayClass);
		FixtureBlock& displayNameMap = addSnapshotProperty(
			"/Script/Engine.UserDefinedEnum.DisplayNameMap", mapClass);
		FixtureBlock& levels = addSnapshotProperty(
			"/Script/Engine.LevelCollection.Levels", setClass);
		FixtureBlock& creationMethod = addSnapshotProperty(
			"/Script/Engine.ActorComponent.CreationMethod", enumClass);
		FixtureBlock& autoPossessAi = addSnapshotProperty(
			"/Script/Engine.Pawn.AutoPossessAI", enumClass);
		FixtureBlock& functionClass = addPropertyClass(castFunction);
		FixtureBlock& nativeFunction = addSnapshotObject(
			"/Script/Engine.Engine.DoThing",
			EngineObjectKind::Function);
		initializeProperty(
			nativeFunction,
			snapshot.Objects.back().Handle.Index,
			functionClass);
		FixtureBlock& functionParameter = addSnapshotProperty(
			"/Script/Engine.Engine.DoThing.Value",
			intClass);
		FixtureBlock& scriptFunction = addSnapshotObject(
			"/Script/Engine.Engine.ScriptThing",
			EngineObjectKind::Function);
		initializeProperty(
			scriptFunction,
			snapshot.Objects.back().Handle.Index,
			functionClass);

		const auto addInnerProperty = [&](FixtureBlock& propertyClass)
			-> FixtureBlock& {
			FixtureBlock& property = allocateBlock();
			const ObjectIdentity identity = addIdentity(property);
			initializeProperty(property, identity.Index, propertyClass);
			return property;
		};
		FixtureBlock& arrayInner = addSnapshotProperty(
			"/Script/Engine.GameViewportClient.DebugProperties.Inner",
			structClass);
		FixtureBlock& localPlayerInner = addSnapshotProperty(
			"/Script/Engine.GameViewportClient.LocalPlayers.Inner",
			objectClass);
		FixtureBlock& mapKey = addInnerProperty(nameClass);
		FixtureBlock& mapValue = addInnerProperty(textClass);
		FixtureBlock& setElement = addInnerProperty(objectClass);
		FixtureBlock& enumUnderlying1 = addInnerProperty(byteClass);
		FixtureBlock& enumUnderlying2 = addInnerProperty(byteClass);

		const auto addressOf = [&](const std::string_view path) {
			const auto found = nodes.find(std::string(path));
			Require(found != nodes.end() && found->second.Memory,
				"Reflection fixture object path is missing");
			return found->second.Memory->Address();
		};
		FixtureBlock& ueStruct = *nodes.at("/Script/CoreUObject.Struct").Memory;
		FixtureBlock& ueField = *nodes.at("/Script/CoreUObject.Field").Memory;
		FixtureBlock& ueClass = *nodes.at("/Script/CoreUObject.Class").Memory;
		FixtureBlock& guid = *nodes.at("/Script/CoreUObject.Guid").Memory;
		FixtureBlock& color = *nodes.at("/Script/CoreUObject.Color").Memory;

		write(ueStruct, structPropertiesSizeOffset, structContainerSize);
		write(guid, structPropertiesSizeOffset, std::int32_t{16});
		write(color, structPropertiesSizeOffset, std::int32_t{4});
		write(guid, structMinAlignmentOffset, std::int32_t{4});
		write(color, structMinAlignmentOffset, std::int32_t{1});
		write(ueStruct, structSuperOffset, ueField.Address());
		write(ueClass, structSuperOffset, ueStruct.Address());
		write(guid, structChildrenOffset, guidA.Address());
		write(color, structChildrenOffset, colorR.Address());
		write(guidA, fieldNextOffset, guidC.Address());
		write(colorR, fieldNextOffset, colorG.Address());
		write(debugProperties, fieldNextOffset, localPlayers.Address());
		write(localPlayers, fieldNextOffset, std::uintptr_t{0});

		for (FixtureBlock* property : std::array{&guidA, &guidC, &guidD})
		{
			write(*property, propertyArrayDimOffset, std::int32_t{1});
			write(*property, propertyElementSizeOffset, std::int32_t{4});
		}
		constexpr std::uint64_t propertyEdit = 0x0000000000000001;
		constexpr std::uint64_t propertyBlueprintVisible = 0x0000000000000004;
		constexpr std::uint64_t propertyZeroConstructor = 0x0000000000000200;
		constexpr std::uint64_t propertySaveGame = 0x0000000001000000;
		constexpr std::uint64_t propertyPlainOldData = 0x0000000040000000;
		constexpr std::uint64_t propertyNoDestructor = 0x0000001000000000;
		constexpr std::uint64_t propertyHasHash = 0x0008000000000000;
		constexpr std::uint64_t guidFlags = propertyEdit
			| propertyZeroConstructor | propertySaveGame | propertyPlainOldData
			| propertyNoDestructor | propertyHasHash;
		constexpr std::uint64_t colorFlags = guidFlags | propertyBlueprintVisible;
		write(guidA, propertyFlagsOffset, guidFlags);
		write(colorR, propertyFlagsOffset, colorFlags);
		write(guidA, propertyOffsetOffset, std::int32_t{0});
		write(guidC, propertyOffsetOffset, std::int32_t{8});
		write(colorB, propertyOffsetOffset, std::int32_t{0});
		write(colorG, propertyOffsetOffset, std::int32_t{1});

		const std::array<std::uint8_t, 4> nativeBoolLayout{1, 0, 1, 0xFF};
		std::memcpy(
			engineBool.Bytes.data() + derivedPropertyOffset,
			nativeBoolLayout.data(),
			nativeBoolLayout.size());
		std::memcpy(
			controllerBool.Bytes.data() + derivedPropertyOffset,
			nativeBoolLayout.data(),
			nativeBoolLayout.size());
		write(traceChannel1, derivedPropertyOffset,
			addressOf("/Script/Engine.ECollisionResponse"));
		write(traceChannel2, derivedPropertyOffset,
			addressOf("/Script/Engine.ECollisionResponse"));
		write(controllerPlayerState, derivedPropertyOffset,
			addressOf("/Script/Engine.PlayerState"));
		write(controllerPawn, derivedPropertyOffset,
			addressOf("/Script/Engine.Pawn"));
		write(twoVectorsV1, derivedPropertyOffset,
			addressOf("/Script/CoreUObject.Vector"));
		write(twoVectorsV2, derivedPropertyOffset,
			addressOf("/Script/CoreUObject.Vector"));
		write(debugProperties, derivedPropertyOffset, arrayInner.Address());
		write(arrayInner, derivedPropertyOffset,
			addressOf("/Script/Engine.DebugDisplayProperty"));
		write(localPlayers, derivedPropertyOffset, localPlayerInner.Address());
		write(localPlayerInner, derivedPropertyOffset,
			addressOf("/Script/Engine.LocalPlayer"));
		write(displayNameMap, derivedPropertyOffset, mapKey.Address());
		write(displayNameMap, derivedPropertyOffset + sizeof(std::uintptr_t),
			mapValue.Address());
		write(levels, derivedPropertyOffset, setElement.Address());
		write(setElement, derivedPropertyOffset,
			addressOf("/Script/Engine.Level"));
		write(creationMethod, derivedPropertyOffset, enumUnderlying1.Address());
		write(creationMethod, derivedPropertyOffset + sizeof(std::uintptr_t),
			addressOf("/Script/Engine.EComponentCreationMethod"));
		write(autoPossessAi, derivedPropertyOffset, enumUnderlying2.Address());
		write(autoPossessAi, derivedPropertyOffset + sizeof(std::uintptr_t),
			addressOf("/Script/Engine.EAutoPossessAI"));

		for (const RequiredObject& required : requiredObjects)
		{
			if (required.Kind == EngineObjectKind::Enum)
				continue;
			FixtureBlock& object = *nodes.at(std::string(required.Path)).Memory;
			std::int32_t propertiesSize = required.Kind == EngineObjectKind::Class
				? 0x100
				: 0x20;
			std::int32_t minAlignment = 8;
			if (required.Path == "/Script/CoreUObject.Field")
				propertiesSize = 0x80;
			else if (required.Path == "/Script/CoreUObject.Struct")
				propertiesSize = structContainerSize;
			else if (required.Path == "/Script/CoreUObject.Class")
				propertiesSize = 0xC0;
			else if (required.Path == "/Script/CoreUObject.Guid")
			{
				propertiesSize = 16;
				minAlignment = 4;
			}
			else if (required.Path == "/Script/CoreUObject.Color")
			{
				propertiesSize = 4;
				minAlignment = 1;
			}
			write(object, structPropertiesSizeOffset, propertiesSize);
			write(object, structMinAlignmentOffset, minAlignment);
		}
		for (FixtureBlock* property : std::array{&colorR, &colorG})
		{
			write(*property, propertyArrayDimOffset, std::int32_t{1});
			write(*property, propertyElementSizeOffset, std::int32_t{1});
		}
		for (FixtureBlock* property : std::array{&debugProperties, &localPlayers})
		{
			write(*property, propertyArrayDimOffset, std::int32_t{1});
			write(*property, propertyElementSizeOffset, std::int32_t{16});
			write(*property, propertyFlagsOffset, std::uint64_t{0});
		}
		write(debugProperties, propertyOffsetOffset, std::int32_t{0x20});
		write(localPlayers, propertyOffsetOffset, std::int32_t{0x30});
		write(arrayInner, propertyArrayDimOffset, std::int32_t{1});
		write(arrayInner, propertyElementSizeOffset, std::int32_t{0x20});
		write(arrayInner, propertyFlagsOffset, std::uint64_t{0});
		write(arrayInner, propertyOffsetOffset, std::int32_t{0});
		write(localPlayerInner, propertyArrayDimOffset, std::int32_t{1});
		write(localPlayerInner, propertyElementSizeOffset, std::int32_t{8});
		write(localPlayerInner, propertyFlagsOffset, std::uint64_t{0});
		write(localPlayerInner, propertyOffsetOffset, std::int32_t{0});
		FixtureBlock& engineType = *nodes.at("/Script/Engine.Engine").Memory;
		write(
			*nodes.at("/Script/Engine.GameViewportClient").Memory,
			structChildrenOffset,
			debugProperties.Address());
		write(engineType, structChildrenOffset, nativeFunction.Address());
		write(nativeFunction, fieldNextOffset, scriptFunction.Address());
		write(nativeFunction, structChildrenOffset, functionParameter.Address());
		write(nativeFunction, structPropertiesSizeOffset, std::int32_t{4});
		write(nativeFunction, std::size_t{0xB0}, std::uint32_t{0x400});
		write(
			nativeFunction,
			std::size_t{0xD8},
			reinterpret_cast<std::uintptr_t>(&FakeProcessEvent));
		write(functionParameter, propertyArrayDimOffset, std::int32_t{1});
		write(functionParameter, propertyElementSizeOffset, std::int32_t{4});
		write(functionParameter, propertyFlagsOffset, std::uint64_t{0x80});
		write(functionParameter, propertyOffsetOffset, std::int32_t{0});
		write(scriptFunction, structPropertiesSizeOffset, std::int32_t{0});
		write(scriptFunction, std::size_t{0xB0}, std::uint32_t{0});
		write(
			scriptFunction,
			std::size_t{0xD8},
			reinterpret_cast<std::uintptr_t>(&FakeProcessEvent));
		const ObjectIdentity functionIdentity = identitySource.Objects.at(
			nodes.at("/Script/Engine.Engine.DoThing").Index);
		const ObjectIdentity scriptFunctionIdentity = identitySource.Objects.at(
			nodes.at("/Script/Engine.Engine.ScriptThing").Index);
		const ObjectIdentity engineIdentity = identitySource.Objects.at(
			nodes.at("/Script/Engine.Engine").Index);
		identitySource.Functions.emplace(
			functionIdentity.Index,
			FunctionIdentity{
				.Function = functionIdentity,
				.Owner = engineIdentity,
				.FullPath = "Function fname:1:0.fname:2:0",
				.SignatureFingerprint = 0x12345678ULL
			});
		identitySource.Functions.emplace(
			scriptFunctionIdentity.Index,
			FunctionIdentity{
				.Function = scriptFunctionIdentity,
				.Owner = engineIdentity,
				.FullPath = "Function fname:1:0.fname:3:0",
				.SignatureFingerprint = 0x87654321ULL
			});

		identitySource.ObjectCount = nextIndex;
		snapshot.SourceObjectCount = nextIndex;
		snapshot.SkippedSlots = static_cast<std::uint32_t>(
			static_cast<std::size_t>(nextIndex) - snapshot.Objects.size());

		const auto mismatchedContext = MakeEngineContext(
			contextGeneration, true, true, nullptr, 4242, true);
		EngineFacade mismatchedFacade(
			mismatchedContext,
			std::string(sessionId),
			identitySource);
		Require(
			mismatchedFacade.Snapshots().Publish(snapshot).Ok(),
			"Reflection mismatch fixture snapshot was rejected");
		ObjectSnapshotReflectionCandidateSource mismatchedSource(
			mismatchedContext,
			mismatchedFacade);
		const ReflectionCandidatePreparationResult mismatch =
			mismatchedSource.Prepare();
		Require(
			mismatch.Error == ReflectionCandidatePreparationError::PropertySystemMismatch
				&& !mismatchedSource.IsConfigured()
				&& mismatchedFacade.Stop(),
			"Reflection source ignored the immutable property-system profile");

		const auto context = MakeEngineContext(
			contextGeneration, true, true, nullptr, 4242, false);
		EngineFacade facade(context, std::string(sessionId), identitySource);
		Require(
			facade.Snapshots().Publish(std::move(snapshot)).Ok(),
			"Production reflection source fixture snapshot was rejected");
		ObjectSnapshotReflectionCandidateSource source(context, facade);
		const ReflectionCandidatePreparationResult prepared = source.Prepare();
		Require(
			prepared.Ok()
				&& prepared.SnapshotGeneration == 1
				&& prepared.PropertySystem == ReflectionPropertySystem::UProperty
				&& source.IsConfigured()
				&& facade.ConfigureReflectionCapture(source, true),
			"Production reflection source did not prepare one immutable snapshot plan");
		ReflectionLayoutCapture* capture = facade.ReflectionCapture();
		Require(
			capture
				&& capture->RequestCapture() == ReflectionLayoutCaptureError::None
				&& capture->Pump(1).Status == ReflectionLayoutPumpStatus::Progress
				&& !facade.Reflection(),
			"Production reflection source published before bounded discovery completed");

		ReflectionLayoutPumpResult finalPump;
		for (std::size_t pump = 0; pump < 64; ++pump)
		{
			finalPump = capture->Pump(ReflectionLayoutCapture::kMaxPumpBudget);
			if (finalPump.Status == ReflectionLayoutPumpStatus::Published
				|| finalPump.Status == ReflectionLayoutPumpStatus::Failed)
			{
				break;
			}
		}
		const ReflectionLayoutCaptureDiagnostics captureDiagnostics =
			capture->Diagnostics();
		const ObjectSnapshotReflectionSourceDiagnostics sourceDiagnostics =
			source.Diagnostics();
		const std::shared_ptr<const ReflectionRuntimeSnapshot> reflection =
			facade.Reflection();
		const auto hasOffset = [&](const ReflectionField field,
			const std::int32_t expected) {
			const ReflectionFieldReport* report = reflection && reflection->Layout
				? reflection->Layout->Find(field)
				: nullptr;
			return report && report->Offset == expected && report->Validated;
		};
		if (finalPump.Status != ReflectionLayoutPumpStatus::Published)
		{
			std::ostringstream detail;
			detail << "Production reflection source failed: pump="
				<< static_cast<int>(finalPump.Status)
				<< ", capture_state=" << static_cast<int>(captureDiagnostics.State)
				<< ", capture_error=" << ToString(captureDiagnostics.Error)
				<< ", source_error=" << ToString(captureDiagnostics.SourceError)
				<< ", validation_error=" << ToString(captureDiagnostics.ValidationError)
				<< ", phase=" << sourceDiagnostics.DiscoveryPhase
				<< ", steps=" << sourceDiagnostics.SourceSteps
				<< ", fields=" << sourceDiagnostics.EmittedFields;
			throw std::runtime_error(detail.str());
		}
		Require(
			captureDiagnostics.State == ReflectionLayoutCaptureState::Completed
				&& captureDiagnostics.Error == ReflectionLayoutCaptureError::None
				&& captureDiagnostics.SourceSteps <= ReflectionLayoutCapture::kMaxSourceSteps
				&& sourceDiagnostics.PreparationError
					== ReflectionCandidatePreparationError::None
				&& sourceDiagnostics.SourceError == ReflectionCandidateSourceError::None
				&& sourceDiagnostics.PropertySystem == ReflectionPropertySystem::UProperty
				&& sourceDiagnostics.EmittedFields == 22
				&& !sourceDiagnostics.Active
				&& reflection
				&& reflection->Layout
				&& reflection->IsPropertyCodecConfigured(contextGeneration)
				&& reflection->Properties->Supports(PropertyKind::Int32)
				&& reflection->Properties->Supports(PropertyKind::Array)
				&& reflection->Properties->Supports(PropertyKind::String)
				&& reflection->Layout->PropertySystem() == ReflectionPropertySystem::UProperty
				&& hasOffset(ReflectionField::StructSuper, structSuperOffset)
				&& hasOffset(ReflectionField::StructChildren, structChildrenOffset)
				&& hasOffset(ReflectionField::StructPropertiesSize, structPropertiesSizeOffset)
				&& hasOffset(ReflectionField::StructMinAlignment, structMinAlignmentOffset)
				&& hasOffset(ReflectionField::UFieldNext, fieldNextOffset)
				&& hasOffset(ReflectionField::PropertyArrayDim, propertyArrayDimOffset)
				&& hasOffset(ReflectionField::PropertyElementSize, propertyElementSizeOffset)
				&& hasOffset(ReflectionField::PropertyFlags, propertyFlagsOffset)
				&& hasOffset(ReflectionField::PropertyOffset, propertyOffsetOffset)
				&& hasOffset(ReflectionField::BoolFieldSize, derivedPropertyOffset)
				&& hasOffset(ReflectionField::MapPropertyValue,
					derivedPropertyOffset + static_cast<std::int32_t>(sizeof(std::uintptr_t))),
			"Production reflection source did not publish the witnessed immutable layout");
		Require(
			source.ReleasePreparedPlan() && !source.IsConfigured(),
			"Production reflection source retained its completed worker-owned plan");

		ObjectSnapshotTypeCandidateSource typeSource(context, facade);
		const TypeCandidatePreparationResult typePrepared = typeSource.Prepare();
		Require(
			typePrepared.Ok()
				&& typePrepared.SnapshotGeneration == 1
				&& typePrepared.ReflectionLayoutFingerprint == reflection->Layout->Fingerprint()
				&& typePrepared.TypeCount == requiredObjects.size()
				&& typePrepared.FunctionCount == 2
				&& typeSource.IsConfigured()
				&& facade.ConfigureTypeSnapshotCapture(typeSource),
			"Production type source did not freeze the exact object/reflection plan");
		TypeSnapshotCapture* typeCapture = facade.TypeCapture();
		Require(
			typeCapture
				&& typeCapture->RequestCapture() == TypeSnapshotCaptureError::None,
			"Production type source capture request was rejected");
		TypeSnapshotPumpResult finalTypePump;
		for (std::size_t pump = 0; pump < 1024; ++pump)
		{
			finalTypePump = typeCapture->Pump(TypeSnapshotCapture::kMaxPumpBudget);
			if (finalTypePump.Status == TypeSnapshotPumpStatus::Ready
				|| finalTypePump.Status == TypeSnapshotPumpStatus::Failed)
			{
				break;
			}
		}
		if (finalTypePump.Status != TypeSnapshotPumpStatus::Ready)
		{
			const TypeSnapshotCaptureDiagnostics diagnostics = typeCapture->Diagnostics();
			const ObjectSnapshotTypeSourceDiagnostics sourceState = typeSource.Diagnostics();
			std::ostringstream detail;
			detail << "Production type source failed: pump="
				<< static_cast<int>(finalTypePump.Status)
				<< ", capture=" << ToString(diagnostics.Error)
				<< ", source=" << ToString(diagnostics.SourceError)
				<< ", phase=" << sourceState.CapturePhase
				<< ", steps=" << sourceState.SourceSteps
				<< ", validation=" << sourceState.ValidationSteps
				<< ", evidence=" << sourceState.CapturedEvidence;
			throw std::runtime_error(detail.str());
		}
		identitySource.ExecutionThreadValid = false;
		const TypeSnapshotPublishResult publishedTypes = typeCapture->PublishReady();
		identitySource.ExecutionThreadValid = true;
		const std::shared_ptr<const TypeSnapshot> types = facade.Types().Current();
		const ReflectedType* guidType = types
			? types->FindByFullPath("/Script/CoreUObject.Guid")
			: nullptr;
		const ReflectedType* engineTypeRecord = types
			? types->FindByFullPath("/Script/Engine.Engine")
			: nullptr;
		const ReflectedType* enumType = types
			? types->FindByFullPath("/Script/Engine.ECollisionResponse")
			: nullptr;
		const ReflectedType* gameViewportType = types
			? types->FindByFullPath("/Script/Engine.GameViewportClient")
			: nullptr;
		const ObjectSnapshotTypeSourceDiagnostics completedTypeSource =
			typeSource.Diagnostics();
		if (!gameViewportType
			|| gameViewportType->DirectProperties.size() != 2
			|| gameViewportType->DirectProperties[1].State != ReflectedMemberState::Supported
			|| !gameViewportType->DirectProperties[1].Descriptor)
		{
			std::ostringstream detail;
			detail << "Object-array descriptor fixture failed: snapshot="
				<< static_cast<bool>(types)
				<< ", publish=" << ToString(publishedTypes.Error)
				<< ", type=" << static_cast<bool>(gameViewportType);
			if (gameViewportType)
			{
				detail << ", properties=" << gameViewportType->DirectProperties.size();
				for (const ReflectedProperty& property : gameViewportType->DirectProperties)
				{
					detail << ", [" << property.Name << ':' << ToString(property.State)
						<< ':' << property.ReasonCode << ']';
				}
			}
			throw std::runtime_error(detail.str());
		}
		Require(
			publishedTypes.Ok()
				&& types == publishedTypes.Snapshot
				&& types->ObjectSnapshotGeneration() == 1
				&& types->Types().size() == requiredObjects.size()
				&& guidType
				&& guidType->DirectProperties.size() == 2
				&& guidType->DirectProperties[0].Name == "A"
				&& guidType->DirectProperties[1].Name == "C"
				&& guidType->DirectProperties[0].State
					== ReflectedMemberState::Supported
				&& guidType->DirectProperties[0].Descriptor
				&& guidType->DirectProperties[0].Descriptor->Kind == PropertyKind::Int32
				&& guidType->DirectProperties[0].Descriptor->Size == 4
				&& engineTypeRecord
				&& engineTypeRecord->DefaultObjectState
					== ClassDefaultObjectState::NotConstructed
				&& engineTypeRecord->DirectFunctions.size() == 2
				&& engineTypeRecord->DirectFunctions[0].Implementation
					== ReflectedFunctionImplementation::Native
				&& engineTypeRecord->DirectFunctions[0].Parameters.size() == 1
				&& engineTypeRecord->DirectFunctions[0].Parameters[0].Direction
					== ReflectedParameterDirection::Input
				&& engineTypeRecord->DirectFunctions[0].Parameters[0].Property.State
					== ReflectedMemberState::Supported
				&& engineTypeRecord->DirectFunctions[0].Parameters[0].Property.Descriptor
				&& engineTypeRecord->DirectFunctions[1].Implementation
					== ReflectedFunctionImplementation::Unavailable
				&& engineTypeRecord->DirectFunctions[1].NativeAddress == 0
				&& engineTypeRecord->DirectFunctions[1].ReasonCode
					== "FUNCTION_BYTECODE_NOT_CAPTURED"
				&& gameViewportType
				&& gameViewportType->DirectProperties.size() == 2
				&& gameViewportType->DirectProperties[0].Name == "DebugProperties"
				&& gameViewportType->DirectProperties[0].State
					== ReflectedMemberState::Unavailable
				&& gameViewportType->DirectProperties[1].Name == "LocalPlayers"
				&& gameViewportType->DirectProperties[1].State
					== ReflectedMemberState::Supported
				&& gameViewportType->DirectProperties[1].Descriptor
				&& gameViewportType->DirectProperties[1].Descriptor->Kind
					== PropertyKind::Array
				&& gameViewportType->DirectProperties[1].Descriptor->ElementStride
					== sizeof(std::uintptr_t)
				&& gameViewportType->DirectProperties[1].Descriptor->Element
				&& gameViewportType->DirectProperties[1].Descriptor->Element->Kind
					== PropertyKind::Object
				&& gameViewportType->DirectProperties[1].Descriptor->Element->TypeName
					== "/Script/Engine.LocalPlayer"
				&& enumType
				&& enumType->EnumState == ReflectedMemberState::Unavailable
				&& completedTypeSource.SourceError == TypeSnapshotSourceError::None
				&& !completedTypeSource.Active
				&& completedTypeSource.CapturedEvidence > requiredObjects.size()
				&& typeSource.ReleasePreparedPlan(),
			"Production type source did not publish witnessed structural metadata");

		const TypeCandidatePreparationResult retryPrepared = typeSource.Prepare();
		Require(
			retryPrepared.Ok()
				&& typeCapture->RequestCapture() == TypeSnapshotCaptureError::None,
			"Production type source could not prepare a second exact-generation audit");
		for (std::size_t pump = 0; pump < 4096
			&& typeCapture->Diagnostics().State != TypeSnapshotCaptureState::Validating
			&& typeCapture->Diagnostics().State != TypeSnapshotCaptureState::Failed;
			++pump)
		{
			(void)typeCapture->Pump(1);
		}
		Require(
			typeCapture->Diagnostics().State == TypeSnapshotCaptureState::Validating,
			"Production type source did not reach incremental validation");
		(void)typeCapture->Pump(1);
		write(guidC, propertyElementSizeOffset, std::int32_t{8});
		for (std::size_t pump = 0; pump < 4096
			&& typeCapture->Diagnostics().State != TypeSnapshotCaptureState::Failed;
			++pump)
		{
			(void)typeCapture->Pump(1);
		}
		write(guidC, propertyElementSizeOffset, std::int32_t{4});
		Require(
			typeCapture->Diagnostics().State == TypeSnapshotCaptureState::Failed
				&& typeCapture->Diagnostics().SourceError
					== TypeSnapshotSourceError::DependencyChanged
				&& typeCapture->ReclaimRetired() == 1
				&& typeSource.ReleasePreparedPlan()
				&& facade.Stop(),
			"Production type source accepted metadata that changed during validation");
	}

	void TestFPropertySnapshotReflectionCandidateSource()
	{
		using namespace UExplorer::Runtime;

		constexpr std::uint64_t contextGeneration = 78;
		constexpr std::string_view sessionId = "fixture-fproperty-reflection-source";
		constexpr std::int32_t structSuperOffset = 0x40;
		constexpr std::int32_t structChildPropertiesOffset = 0x60;
		constexpr std::int32_t structPropertiesSizeOffset = 0x58;
		constexpr std::int32_t structMinAlignmentOffset = 0x5C;
		constexpr std::int32_t fieldClassOffset = 0x08;
		constexpr std::int32_t fieldNextOffset = 0x20;
		constexpr std::int32_t fieldNameOffset = 0x28;
		constexpr std::int32_t fieldClassCastFlagsOffset = 0x08;
		constexpr std::int32_t propertyArrayDimOffset = 0x38;
		constexpr std::int32_t propertyElementSizeOffset = 0x3C;
		constexpr std::int32_t propertyFlagsOffset = 0x40;
		constexpr std::int32_t propertyOffsetOffset = 0x4C;
		constexpr std::int32_t derivedPropertyOffset = 0x70;

		constexpr std::uint64_t castField = 0x0000000000000001;
		constexpr std::uint64_t castByte = 0x0000000000000040;
		constexpr std::uint64_t castInt = 0x0000000000000080;
		constexpr std::uint64_t castName = 0x0000000000002000;
		constexpr std::uint64_t castProperty = 0x0000000000008000;
		constexpr std::uint64_t castObject = 0x0000000000010000;
		constexpr std::uint64_t castBool = 0x0000000000020000;
		constexpr std::uint64_t castStruct = 0x0000000000100000;
		constexpr std::uint64_t castArray = 0x0000000000200000;
		constexpr std::uint64_t castNumeric = 0x0000000001000000;
		constexpr std::uint64_t castText = 0x0000000040000000;
		constexpr std::uint64_t castMap = 0x0000400000000000;
		constexpr std::uint64_t castSet = 0x0000800000000000;
		constexpr std::uint64_t castEnum = 0x0001000000000000;

		struct alignas(16) FixtureBlock final
		{
			std::array<std::byte, 0x240> Bytes{};

			std::uintptr_t Address() noexcept
			{
				return reinterpret_cast<std::uintptr_t>(Bytes.data());
			}
		};
		struct FixtureNode final
		{
			FixtureBlock* Memory = nullptr;
			std::int32_t Index = -1;
		};

		std::vector<std::unique_ptr<FixtureBlock>> memory;
		const auto allocateBlock = [&]() -> FixtureBlock& {
			memory.push_back(std::make_unique<FixtureBlock>());
			return *memory.back();
		};
		const auto write = []<typename T>(
			FixtureBlock& block,
			const std::size_t offset,
			const T& value) {
			static_assert(std::is_trivially_copyable_v<T>);
			Require(
				offset <= block.Bytes.size()
					&& sizeof(T) <= block.Bytes.size() - offset,
				"FProperty fixture write exceeded its backing block");
			std::memcpy(block.Bytes.data() + offset, &value, sizeof(T));
		};

		std::array<std::byte, 64> namePool{};
		std::array<std::byte, 4096> nameBlock{};
		const auto writeNameBytes = [](auto& buffer,
			const std::size_t offset,
			const void* value,
			const std::size_t size) {
			Require(
				offset <= buffer.size() && size <= buffer.size() - offset,
				"FProperty name fixture exceeded its backing block");
			std::memcpy(buffer.data() + offset, value, size);
		};
		std::size_t nextNameOffset = 2;
		std::unordered_map<std::string, std::uint32_t> nameIndexes;
		const auto addName = [&](const std::string_view name) {
			const auto existing = nameIndexes.find(std::string(name));
			if (existing != nameIndexes.end())
				return existing->second;
			Require(name.size() <= 1024,
				"FProperty fixture name exceeds the runtime name bound");
			const std::uint16_t header = static_cast<std::uint16_t>(name.size() << 6);
			writeNameBytes(nameBlock, nextNameOffset, &header, sizeof(header));
			writeNameBytes(nameBlock, nextNameOffset + 2, name.data(), name.size());
			const std::uint32_t index = static_cast<std::uint32_t>(nextNameOffset / 2);
			nextNameOffset += 2 + name.size();
			if ((nextNameOffset & 1u) != 0)
				++nextNameOffset;
			nameIndexes.emplace(std::string(name), index);
			return index;
		};
		const std::uintptr_t nameBlockAddress =
			reinterpret_cast<std::uintptr_t>(nameBlock.data());
		std::memcpy(namePool.data() + 16, &nameBlockAddress, sizeof(nameBlockAddress));

		FakeHandleIdentitySource identitySource;
		identitySource.Generation = contextGeneration;
		std::int32_t nextIndex = 1;
		EngineSnapshot snapshot{
			.SessionId = std::string(sessionId),
			.ContextGeneration = contextGeneration,
			.Generation = 1,
			.CapturedAtMonotonicUs = 2000,
			.CaptureDurationUs = 10
		};
		std::unordered_map<std::string, FixtureNode> nodes;

		const auto addSnapshotObject = [&](const std::string_view path,
			const EngineObjectKind kind) -> FixtureBlock& {
			FixtureBlock& block = allocateBlock();
			const std::int32_t index = nextIndex++;
			const ObjectIdentity identity{
				.Index = index,
				.SerialNumber = 2000 + index,
				.Address = block.Address(),
				.ClassFingerprint = 0xB000u + static_cast<std::uint64_t>(index)
			};
			identitySource.Objects.emplace(index, identity);
			const std::string fullPath(path);
			const std::size_t nameStart = fullPath.find_last_of('.');
			const std::size_t packageEnd = fullPath.find('.');
			snapshot.Objects.push_back({
				.Handle = {
					.SessionId = std::string(sessionId),
					.ContextGeneration = contextGeneration,
					.Index = identity.Index,
					.SerialNumber = identity.SerialNumber,
					.Address = identity.Address,
					.ClassFingerprint = identity.ClassFingerprint
				},
				.Name = fullPath.substr(nameStart + 1),
				.FullPath = fullPath,
				.ClassPath = "/Script/CoreUObject.Class",
				.PackagePath = fullPath.substr(0, packageEnd),
				.Kind = kind
			});
			nodes.emplace(fullPath, FixtureNode{&block, index});
			return block;
		};

		struct RequiredObject final
		{
			std::string_view Path;
			EngineObjectKind Kind;
		};
		constexpr std::array requiredObjects{
			RequiredObject{"/Script/CoreUObject.Struct", EngineObjectKind::Class},
			RequiredObject{"/Script/CoreUObject.Field", EngineObjectKind::Class},
			RequiredObject{"/Script/CoreUObject.Class", EngineObjectKind::Class},
			RequiredObject{"/Script/CoreUObject.Guid", EngineObjectKind::Struct},
			RequiredObject{"/Script/CoreUObject.Color", EngineObjectKind::Struct},
			RequiredObject{"/Script/CoreUObject.Vector", EngineObjectKind::Struct},
			RequiredObject{"/Script/CoreUObject.TwoVectors", EngineObjectKind::Struct},
			RequiredObject{"/Script/Engine.Engine", EngineObjectKind::Class},
			RequiredObject{"/Script/Engine.PlayerController", EngineObjectKind::Class},
			RequiredObject{"/Script/Engine.CollisionResponseContainer", EngineObjectKind::Struct},
			RequiredObject{"/Script/Engine.Controller", EngineObjectKind::Class},
			RequiredObject{"/Script/Engine.PlayerState", EngineObjectKind::Class},
			RequiredObject{"/Script/Engine.Pawn", EngineObjectKind::Class},
			RequiredObject{"/Script/Engine.GameViewportClient", EngineObjectKind::Class},
			RequiredObject{"/Script/Engine.UserDefinedEnum", EngineObjectKind::Class},
			RequiredObject{"/Script/Engine.LevelCollection", EngineObjectKind::Struct},
			RequiredObject{"/Script/Engine.ActorComponent", EngineObjectKind::Class},
			RequiredObject{"/Script/Engine.DebugDisplayProperty", EngineObjectKind::Struct},
			RequiredObject{"/Script/Engine.Level", EngineObjectKind::Class},
			RequiredObject{"/Script/Engine.ECollisionResponse", EngineObjectKind::Enum},
			RequiredObject{"/Script/Engine.EComponentCreationMethod", EngineObjectKind::Enum},
			RequiredObject{"/Script/Engine.EAutoPossessAI", EngineObjectKind::Enum}
		};
		for (const RequiredObject& required : requiredObjects)
			(void)addSnapshotObject(required.Path, required.Kind);

		const auto addFieldClass = [&](const std::uint64_t castFlags)
			-> FixtureBlock& {
			FixtureBlock& block = allocateBlock();
			write(block, fieldClassCastFlagsOffset, castFlags);
			return block;
		};
		FixtureBlock& intClass = addFieldClass(
			castField | castProperty | castNumeric | castInt);
		FixtureBlock& byteClass = addFieldClass(
			castField | castProperty | castNumeric | castByte);
		FixtureBlock& boolClass = addFieldClass(castField | castProperty | castBool);
		FixtureBlock& objectClass = addFieldClass(castField | castProperty | castObject);
		FixtureBlock& structClass = addFieldClass(castField | castProperty | castStruct);
		FixtureBlock& arrayClass = addFieldClass(castField | castProperty | castArray);
		FixtureBlock& mapClass = addFieldClass(castField | castProperty | castMap);
		FixtureBlock& setClass = addFieldClass(castField | castProperty | castSet);
		FixtureBlock& enumClass = addFieldClass(castField | castProperty | castEnum);
		FixtureBlock& nameClass = addFieldClass(castField | castProperty | castName);
		FixtureBlock& textClass = addFieldClass(castField | castProperty | castText);

		std::unordered_map<std::string, FixtureBlock*> fields;
		const auto addField = [&](const std::string_view path,
			const std::string_view name,
			FixtureBlock& fieldClass) -> FixtureBlock& {
			FixtureBlock& field = allocateBlock();
			write(field, fieldClassOffset, fieldClass.Address());
			const std::array<std::uint32_t, 2> fname{addName(name), 0};
			write(field, fieldNameOffset, fname);
			fields.emplace(std::string(path), &field);
			return field;
		};
		FixtureBlock& guidA = addField("Guid.A", "A", intClass);
		FixtureBlock& guidC = addField("Guid.C", "C", intClass);
		FixtureBlock& guidD = addField("Guid.D", "D", intClass);
		FixtureBlock& colorR = addField("Color.R", "R", byteClass);
		FixtureBlock& colorB = addField("Color.B", "B", byteClass);
		FixtureBlock& colorG = addField("Color.G", "G", byteClass);
		FixtureBlock& engineBool = addField(
			"Engine.bIsOverridingSelectedColor",
			"bIsOverridingSelectedColor",
			boolClass);
		FixtureBlock& controllerBool = addField(
			"PlayerController.bAutoManageActiveCameraTarget",
			"bAutoManageActiveCameraTarget",
			boolClass);
		FixtureBlock& traceChannel1 = addField(
			"CollisionResponseContainer.GameTraceChannel1",
			"GameTraceChannel1",
			byteClass);
		FixtureBlock& traceChannel2 = addField(
			"CollisionResponseContainer.GameTraceChannel2",
			"GameTraceChannel2",
			byteClass);
		FixtureBlock& controllerPlayerState = addField(
			"Controller.PlayerState", "PlayerState", objectClass);
		FixtureBlock& controllerPawn = addField(
			"Controller.Pawn", "Pawn", objectClass);
		FixtureBlock& twoVectorsV1 = addField("TwoVectors.v1", "v1", structClass);
		FixtureBlock& twoVectorsV2 = addField("TwoVectors.v2", "v2", structClass);
		FixtureBlock& debugProperties = addField(
			"GameViewportClient.DebugProperties", "DebugProperties", arrayClass);
		FixtureBlock& displayNameMap = addField(
			"UserDefinedEnum.DisplayNameMap", "DisplayNameMap", mapClass);
		FixtureBlock& levels = addField("LevelCollection.Levels", "Levels", setClass);
		FixtureBlock& creationMethod = addField(
			"ActorComponent.CreationMethod", "CreationMethod", enumClass);
		FixtureBlock& autoPossessAi = addField(
			"Pawn.AutoPossessAI", "AutoPossessAI", enumClass);
		FixtureBlock& arrayInner = addField("Inner.DebugProperty", "Inner", structClass);
		FixtureBlock& mapKey = addField("Inner.DisplayNameKey", "Key", nameClass);
		FixtureBlock& mapValue = addField("Inner.DisplayNameValue", "Value", textClass);
		FixtureBlock& setElement = addField("Inner.Level", "Element", objectClass);
		FixtureBlock& enumUnderlying1 = addField("Inner.CreationMethod", "Underlying", byteClass);
		FixtureBlock& enumUnderlying2 = addField("Inner.AutoPossessAI", "Underlying2", byteClass);

		const auto link = [&](FixtureBlock& left, FixtureBlock* right) {
			write(left, fieldNextOffset, right ? right->Address() : std::uintptr_t{0});
		};
		link(guidA, &guidC);
		link(guidC, &guidD);
		link(guidD, nullptr);
		link(colorR, &colorB);
		link(colorB, &colorG);
		link(colorG, nullptr);
		link(engineBool, nullptr);
		link(controllerBool, nullptr);
		link(traceChannel1, &traceChannel2);
		link(traceChannel2, nullptr);
		link(controllerPlayerState, &controllerPawn);
		link(controllerPawn, nullptr);
		link(twoVectorsV1, &twoVectorsV2);
		link(twoVectorsV2, nullptr);
		link(debugProperties, nullptr);
		link(displayNameMap, nullptr);
		link(levels, nullptr);
		link(creationMethod, nullptr);
		link(autoPossessAi, nullptr);

		const auto object = [&](const std::string_view path) -> FixtureBlock& {
			return *nodes.at(std::string(path)).Memory;
		};
		FixtureBlock& ueStruct = object("/Script/CoreUObject.Struct");
		FixtureBlock& ueField = object("/Script/CoreUObject.Field");
		FixtureBlock& ueClass = object("/Script/CoreUObject.Class");
		FixtureBlock& guid = object("/Script/CoreUObject.Guid");
		FixtureBlock& color = object("/Script/CoreUObject.Color");
		write(ueStruct, structPropertiesSizeOffset, std::int32_t{0xA0});
		write(guid, structPropertiesSizeOffset, std::int32_t{16});
		write(color, structPropertiesSizeOffset, std::int32_t{4});
		write(guid, structMinAlignmentOffset, std::int32_t{4});
		write(color, structMinAlignmentOffset, std::int32_t{1});
		write(ueStruct, structSuperOffset, ueField.Address());
		write(ueClass, structSuperOffset, ueStruct.Address());
		write(guid, structChildPropertiesOffset, guidA.Address());
		write(color, structChildPropertiesOffset, colorR.Address());
		write(object("/Script/Engine.Engine"),
			structChildPropertiesOffset, engineBool.Address());
		write(object("/Script/Engine.PlayerController"),
			structChildPropertiesOffset, controllerBool.Address());
		write(object("/Script/Engine.CollisionResponseContainer"),
			structChildPropertiesOffset, traceChannel1.Address());
		write(object("/Script/Engine.Controller"),
			structChildPropertiesOffset, controllerPlayerState.Address());
		write(object("/Script/CoreUObject.TwoVectors"),
			structChildPropertiesOffset, twoVectorsV1.Address());
		write(object("/Script/Engine.GameViewportClient"),
			structChildPropertiesOffset, debugProperties.Address());
		write(object("/Script/Engine.UserDefinedEnum"),
			structChildPropertiesOffset, displayNameMap.Address());
		write(object("/Script/Engine.LevelCollection"),
			structChildPropertiesOffset, levels.Address());
		write(object("/Script/Engine.ActorComponent"),
			structChildPropertiesOffset, creationMethod.Address());
		write(object("/Script/Engine.Pawn"),
			structChildPropertiesOffset, autoPossessAi.Address());

		for (FixtureBlock* property : std::array{&guidA, &guidC, &guidD})
		{
			write(*property, propertyArrayDimOffset, std::int32_t{1});
			write(*property, propertyElementSizeOffset, std::int32_t{4});
		}
		constexpr std::uint64_t propertyEdit = 0x0000000000000001;
		constexpr std::uint64_t propertyBlueprintVisible = 0x0000000000000004;
		constexpr std::uint64_t propertyZeroConstructor = 0x0000000000000200;
		constexpr std::uint64_t propertySaveGame = 0x0000000001000000;
		constexpr std::uint64_t propertyPlainOldData = 0x0000000040000000;
		constexpr std::uint64_t propertyNoDestructor = 0x0000001000000000;
		constexpr std::uint64_t propertyHasHash = 0x0008000000000000;
		constexpr std::uint64_t guidFlags = propertyEdit
			| propertyZeroConstructor | propertySaveGame | propertyPlainOldData
			| propertyNoDestructor | propertyHasHash;
		constexpr std::uint64_t colorFlags = guidFlags | propertyBlueprintVisible;
		write(guidA, propertyFlagsOffset, guidFlags);
		write(colorR, propertyFlagsOffset, colorFlags);
		write(guidA, propertyOffsetOffset, std::int32_t{0});
		write(guidC, propertyOffsetOffset, std::int32_t{8});
		write(colorB, propertyOffsetOffset, std::int32_t{0});
		write(colorG, propertyOffsetOffset, std::int32_t{1});

		const std::array<std::uint8_t, 4> nativeBoolLayout{1, 0, 1, 0xFF};
		std::memcpy(engineBool.Bytes.data() + derivedPropertyOffset,
			nativeBoolLayout.data(), nativeBoolLayout.size());
		std::memcpy(controllerBool.Bytes.data() + derivedPropertyOffset,
			nativeBoolLayout.data(), nativeBoolLayout.size());
		write(traceChannel1, derivedPropertyOffset,
			object("/Script/Engine.ECollisionResponse").Address());
		write(traceChannel2, derivedPropertyOffset,
			object("/Script/Engine.ECollisionResponse").Address());
		write(controllerPlayerState, derivedPropertyOffset,
			object("/Script/Engine.PlayerState").Address());
		write(controllerPawn, derivedPropertyOffset,
			object("/Script/Engine.Pawn").Address());
		write(twoVectorsV1, derivedPropertyOffset,
			object("/Script/CoreUObject.Vector").Address());
		write(twoVectorsV2, derivedPropertyOffset,
			object("/Script/CoreUObject.Vector").Address());
		write(debugProperties, derivedPropertyOffset, arrayInner.Address());
		write(arrayInner, derivedPropertyOffset,
			object("/Script/Engine.DebugDisplayProperty").Address());
		write(displayNameMap, derivedPropertyOffset, mapKey.Address());
		write(displayNameMap, derivedPropertyOffset + sizeof(std::uintptr_t),
			mapValue.Address());
		write(levels, derivedPropertyOffset, setElement.Address());
		write(setElement, derivedPropertyOffset,
			object("/Script/Engine.Level").Address());
		write(creationMethod, derivedPropertyOffset, enumUnderlying1.Address());
		write(creationMethod, derivedPropertyOffset + sizeof(std::uintptr_t),
			object("/Script/Engine.EComponentCreationMethod").Address());
		write(autoPossessAi, derivedPropertyOffset, enumUnderlying2.Address());
		write(autoPossessAi, derivedPropertyOffset + sizeof(std::uintptr_t),
			object("/Script/Engine.EAutoPossessAI").Address());

		const std::int32_t currentBlock = 0;
		const std::int32_t byteCursor = static_cast<std::int32_t>(nextNameOffset);
		std::memcpy(namePool.data(), &currentBlock, sizeof(currentBlock));
		std::memcpy(namePool.data() + 4, &byteCursor, sizeof(byteCursor));
		const EngineNameProfile nameProfile{
			.Storage = EngineNameStorageKind::NamePool,
			.StorageAddress = reinterpret_cast<std::uintptr_t>(namePool.data()),
			.FNameSize = 8,
			.ComparisonIndexOffset = 0,
			.NumberOffset = 4,
			.BlockOffsetBits = 14,
			.EntryStride = 2,
			.ChunksStart = 16,
			.MaxChunkIndexOffset = 0,
			.ByteCursorOffset = 4,
			.EntryStringOffset = 2,
			.EntryHeaderOffset = 0,
			.EntryLengthShift = 6,
			.Validated = true,
			.Source = "fproperty-reflection-fixture"
		};
		identitySource.ObjectCount = nextIndex;
		snapshot.SourceObjectCount = nextIndex;
		snapshot.SkippedSlots = static_cast<std::uint32_t>(
			static_cast<std::size_t>(nextIndex) - snapshot.Objects.size());

		const auto context = MakeEngineContext(
			contextGeneration, true, true, &nameProfile, 4242, true);
		EngineFacade facade(context, std::string(sessionId), identitySource);
		Require(
			facade.Snapshots().Publish(std::move(snapshot)).Ok(),
			"FProperty reflection fixture snapshot was rejected");
		ObjectSnapshotReflectionCandidateSource source(context, facade);
		const ReflectionCandidatePreparationResult prepared = source.Prepare();
		Require(
			prepared.Ok()
				&& prepared.PropertySystem == ReflectionPropertySystem::FProperty
				&& facade.ConfigureReflectionCapture(source, true),
			"FProperty reflection source did not prepare its immutable plan");
		ReflectionLayoutCapture* capture = facade.ReflectionCapture();
		Require(capture
				&& capture->RequestCapture() == ReflectionLayoutCaptureError::None,
			"FProperty reflection capture request was rejected");
		ReflectionLayoutPumpResult finalPump;
		for (std::size_t pump = 0; pump < 64; ++pump)
		{
			finalPump = capture->Pump(ReflectionLayoutCapture::kMaxPumpBudget);
			if (finalPump.Status == ReflectionLayoutPumpStatus::Published
				|| finalPump.Status == ReflectionLayoutPumpStatus::Failed)
			{
				break;
			}
		}
		const ReflectionLayoutCaptureDiagnostics captureDiagnostics =
			capture->Diagnostics();
		const ObjectSnapshotReflectionSourceDiagnostics sourceDiagnostics =
			source.Diagnostics();
		if (finalPump.Status != ReflectionLayoutPumpStatus::Published)
		{
			std::ostringstream detail;
			detail << "FProperty reflection source failed: capture_error="
				<< ToString(captureDiagnostics.Error)
				<< ", source_error=" << ToString(captureDiagnostics.SourceError)
				<< ", validation_error=" << ToString(captureDiagnostics.ValidationError)
				<< ", phase=" << sourceDiagnostics.DiscoveryPhase
				<< ", steps=" << sourceDiagnostics.SourceSteps
				<< ", fields=" << sourceDiagnostics.EmittedFields;
			throw std::runtime_error(detail.str());
		}
		const std::shared_ptr<const ReflectionRuntimeSnapshot> reflection =
			facade.Reflection();
		const auto hasOffset = [&](const ReflectionField field,
			const std::int32_t expected) {
			const ReflectionFieldReport* report = reflection && reflection->Layout
				? reflection->Layout->Find(field)
				: nullptr;
			return report && report->Offset == expected && report->Validated;
		};
		Require(
			captureDiagnostics.State == ReflectionLayoutCaptureState::Completed
				&& sourceDiagnostics.EmittedFields == 25
				&& reflection
				&& reflection->Layout
				&& reflection->IsPropertyCodecConfigured(contextGeneration)
				&& reflection->Properties->Supports(PropertyKind::Name)
				&& reflection->Properties->Supports(PropertyKind::Array)
				&& !reflection->Properties->Supports(PropertyKind::Set)
				&& reflection->Layout->PropertySystem() == ReflectionPropertySystem::FProperty
				&& hasOffset(ReflectionField::StructSuper, structSuperOffset)
				&& hasOffset(ReflectionField::StructChildProperties,
					structChildPropertiesOffset)
				&& hasOffset(ReflectionField::FFieldClass, fieldClassOffset)
				&& hasOffset(ReflectionField::FFieldNext, fieldNextOffset)
				&& hasOffset(ReflectionField::FFieldName, fieldNameOffset)
				&& hasOffset(ReflectionField::FFieldClassCastFlags,
					fieldClassCastFlagsOffset)
				&& hasOffset(ReflectionField::PropertyArrayDim, propertyArrayDimOffset)
				&& hasOffset(ReflectionField::PropertyFlags, propertyFlagsOffset)
				&& hasOffset(ReflectionField::BoolFieldSize, derivedPropertyOffset)
				&& source.ReleasePreparedPlan()
				&& !source.IsConfigured()
				&& facade.Stop(),
			"FProperty reflection source did not publish the witnessed layout");
	}

	class FakeCoreStatusDiagnostics final : public UExplorer::Services::ICoreStatusDiagnosticsSource
	{
	public:
		std::string ProcessArchitecture() const override { return "x64-fixture"; }

		UExplorer::Services::ScriptOffsetDiagnostics CaptureScriptOffsetDiagnostics() const override
		{
			return {
				.SelectedOffset = 0x60,
				.SelectedScore = 100,
				.Confidence = "fixture",
				.AnomalyTags = "none"
			};
		}
	};

	UExplorer::Runtime::EngineSnapshotObject MakeSnapshotObject(
		const std::string& sessionId,
		const std::uint64_t contextGeneration,
		const std::int32_t index,
		const UExplorer::Runtime::EngineObjectKind kind = UExplorer::Runtime::EngineObjectKind::Object)
	{
		return {
			.Handle = {
				.SessionId = sessionId,
				.ContextGeneration = contextGeneration,
				.Index = index,
				.SerialNumber = 100 + index,
				.Address = static_cast<std::uintptr_t>(0x1000 + index * 0x100),
				.ClassFingerprint = static_cast<std::uint64_t>(0xA000 + index)
			},
			.Name = "Object" + std::to_string(index),
			.FullPath = "/Script/Fixture.Object" + std::to_string(index),
			.ClassPath = "/Script/CoreUObject.Object",
			.PackagePath = "/Script/Fixture",
			.Kind = kind
		};
	}

	class FakeSnapshotSource final : public UExplorer::Runtime::IEngineSnapshotSource
	{
	public:
		std::uint64_t Generation = 42;
		std::atomic<bool> ExecutionThreadValid{true};
		std::atomic<bool> BlockCapture{false};
		std::atomic<bool> CaptureEntered{false};
		std::atomic<bool> ThrowOnCount{false};
		std::atomic<std::int32_t> FailReadIndex{-1};
		std::atomic<std::int32_t> FailValidationIndex{-1};
		std::atomic<std::uint32_t> WorkCalls{0};
		std::vector<std::optional<UExplorer::Runtime::EngineSnapshotObject>> Slots;

		std::uint64_t ContextGeneration() const noexcept override { return Generation; }
		bool IsCurrentExecutionThreadValid() const noexcept override
		{
			return ExecutionThreadValid.load(std::memory_order_acquire);
		}

		bool TryGetObjectCount(std::int32_t& objectCount) override
		{
			if (ThrowOnCount.load(std::memory_order_acquire))
				throw std::runtime_error("fixture snapshot count failure");
			if (Slots.size() > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()))
				return false;
			objectCount = static_cast<std::int32_t>(Slots.size());
			return true;
		}

		UExplorer::Runtime::SnapshotSlotReadResult TryCaptureObject(
			const std::int32_t index,
			UExplorer::Runtime::EngineSnapshotObject& object) override
		{
			WorkCalls.fetch_add(1, std::memory_order_relaxed);
			CaptureEntered.store(true, std::memory_order_release);
			while (BlockCapture.load(std::memory_order_acquire))
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			if (index < 0 || static_cast<std::size_t>(index) >= Slots.size()
				|| FailReadIndex.load(std::memory_order_acquire) == index)
			{
				return UExplorer::Runtime::SnapshotSlotReadResult::Failed;
			}
			if (!Slots[static_cast<std::size_t>(index)])
				return UExplorer::Runtime::SnapshotSlotReadResult::Empty;
			object = *Slots[static_cast<std::size_t>(index)];
			return UExplorer::Runtime::SnapshotSlotReadResult::Captured;
		}

		bool ValidateSlot(
			const std::int32_t index,
			const UExplorer::Runtime::EngineSnapshotObject* expectedObject) override
		{
			WorkCalls.fetch_add(1, std::memory_order_relaxed);
			if (index < 0 || static_cast<std::size_t>(index) >= Slots.size()
				|| FailValidationIndex.load(std::memory_order_acquire) == index)
			{
				return false;
			}
			const auto& current = Slots[static_cast<std::size_t>(index)];
			if (!expectedObject)
				return !current.has_value();
			if (!current)
				return false;
			return current->Handle.SessionId == expectedObject->Handle.SessionId
				&& current->Handle.ContextGeneration == expectedObject->Handle.ContextGeneration
				&& current->Handle.Index == expectedObject->Handle.Index
				&& current->Handle.SerialNumber == expectedObject->Handle.SerialNumber
				&& current->Handle.Address == expectedObject->Handle.Address
				&& current->Handle.ClassFingerprint == expectedObject->Handle.ClassFingerprint
				&& current->Name == expectedObject->Name
				&& current->FullPath == expectedObject->FullPath
				&& current->ClassPath == expectedObject->ClassPath
				&& current->PackagePath == expectedObject->PackagePath
				&& current->Kind == expectedObject->Kind;
		}
	};

	void TestProductionSnapshotMetadataSource()
	{
		using namespace UExplorer::Runtime;

		const auto writeValue = [](auto& buffer, const std::size_t offset, const auto& value) {
			Require(offset <= buffer.size() && sizeof(value) <= buffer.size() - offset,
				"Production snapshot fixture write exceeded its buffer");
			std::memcpy(buffer.data() + offset, &value, sizeof(value));
		};
		const auto writeRaw = [](auto& buffer, const std::size_t offset, const void* data, const std::size_t size) {
			Require(offset <= buffer.size() && size <= buffer.size() - offset,
				"Production snapshot string write exceeded its buffer");
			std::memcpy(buffer.data() + offset, data, size);
		};

		std::array<std::byte, 64> pool{};
		std::array<std::byte, 512> nameBlock{};
		std::size_t nameCursor = 0;
		const auto addName = [&](const std::string& value) {
			Require(!value.empty() && value.size() <= 1024, "Snapshot fixture name is invalid");
			nameCursor = (nameCursor + 1) & ~std::size_t{1};
			const std::uint32_t id = static_cast<std::uint32_t>(nameCursor / 2);
			const std::uint16_t header = static_cast<std::uint16_t>(value.size() << 6);
			writeValue(nameBlock, nameCursor, header);
			writeRaw(nameBlock, nameCursor + 2, value.data(), value.size());
			nameCursor = (nameCursor + 2 + value.size() + 1) & ~std::size_t{1};
			return id;
		};

		const std::uint32_t noneName = addName("None");
		const std::uint32_t corePackageName = addName("/Script/CoreUObject");
		const std::uint32_t className = addName("Class");
		const std::uint32_t packageName = addName("Package");
		const std::uint32_t actorName = addName("Actor");
		const std::uint32_t fixturePackageName = addName("/Script/Fixture");
		const std::uint32_t heroName = addName("Hero");
		Require(noneName == 0, "NamePool fixture did not preserve the None witness");
		writeValue(pool, 0, std::int32_t{0});
		writeValue(pool, 4, static_cast<std::int32_t>(nameCursor));
		writeValue(pool, 16, reinterpret_cast<std::uintptr_t>(nameBlock.data()));

		EngineNameProfile nameProfile{
			.Storage = EngineNameStorageKind::NamePool,
			.StorageAddress = reinterpret_cast<std::uintptr_t>(pool.data()),
			.FNameSize = 8,
			.ComparisonIndexOffset = 0,
			.NumberOffset = 4,
			.BlockOffsetBits = 14,
			.EntryStride = 2,
			.ChunksStart = 16,
			.MaxChunkIndexOffset = 0,
			.ByteCursorOffset = 4,
			.EntryStringOffset = 2,
			.EntryHeaderOffset = 0,
			.EntryLengthShift = 6,
			.Validated = true,
			.Source = "snapshot-name-fixture"
		};

		using ObjectBytes = std::array<std::byte, 0x60>;
		ObjectBytes corePackage{};
		ObjectBytes classClass{};
		ObjectBytes packageClass{};
		ObjectBytes actorClass{};
		ObjectBytes fixturePackage{};
		ObjectBytes hero{};
		const auto addressOf = [](ObjectBytes& object) {
			return reinterpret_cast<std::uintptr_t>(object.data());
		};
		const auto setObject = [&](
			ObjectBytes& object,
			const std::int32_t index,
			const std::uintptr_t classAddress,
			const std::uint32_t nameIndex,
			const std::uintptr_t outerAddress) {
			writeValue(object, 0x0C, index);
			writeValue(object, 0x10, classAddress);
			writeValue(object, 0x18, nameIndex);
			writeValue(object, 0x1C, std::uint32_t{0});
			writeValue(object, 0x20, outerAddress);
		};

		setObject(corePackage, 1, addressOf(packageClass), corePackageName, 0);
		setObject(classClass, 2, addressOf(classClass), className, addressOf(corePackage));
		setObject(packageClass, 3, addressOf(classClass), packageName, addressOf(corePackage));
		setObject(actorClass, 4, addressOf(classClass), actorName, addressOf(corePackage));
		setObject(fixturePackage, 5, addressOf(packageClass), fixturePackageName, 0);
		setObject(hero, 6, addressOf(actorClass), heroName, addressOf(fixturePackage));
		writeValue(classClass, 0x38, std::uint64_t{0x0000000000000020ULL});
		writeValue(packageClass, 0x38, std::uint64_t{0x0000000400000000ULL});
		writeValue(actorClass, 0x38, std::uint64_t{0x0000001000000000ULL});

		FakeHandleIdentitySource identitySource;
		identitySource.Generation = 42;
		identitySource.ObjectCount = 7;
		const auto addIdentity = [&](const std::int32_t index, ObjectBytes& object) {
			identitySource.Objects.emplace(index, ObjectIdentity{
				.Index = index,
				.SerialNumber = 1000 + index,
				.Address = addressOf(object),
				.ClassFingerprint = static_cast<std::uint64_t>(0xA000 + index)
			});
		};
		addIdentity(1, corePackage);
		addIdentity(2, classClass);
		addIdentity(3, packageClass);
		addIdentity(4, actorClass);
		addIdentity(5, fixturePackage);
		addIdentity(6, hero);

		const auto context = MakeEngineContext(42, true, true, &nameProfile);
		EngineFacade facade(context, "fixture-production-snapshot", identitySource);
		ObjectArraySnapshotSource source(context, facade, identitySource);
		Require(source.IsConfigured(), "Production snapshot metadata source rejected a complete profile");
		std::int32_t objectCount = -1;
		Require(
			source.TryGetObjectCount(objectCount) && objectCount == 7,
			"Production snapshot source did not expose a checked object count");

		EngineSnapshotObject empty;
		Require(
			source.TryCaptureObject(0, empty) == SnapshotSlotReadResult::Empty,
			"Production snapshot source confused an empty slot with a read failure");
		EngineSnapshotObject heroRecord;
		Require(
			source.TryCaptureObject(6, heroRecord) == SnapshotSlotReadResult::Captured,
			"Production snapshot source failed to capture a coherent object");
		Require(
			heroRecord.Name == "Hero"
				&& heroRecord.FullPath == "/Script/Fixture.Hero"
				&& heroRecord.ClassPath == "/Script/CoreUObject.Actor"
				&& heroRecord.PackagePath == "/Script/Fixture"
				&& heroRecord.Kind == EngineObjectKind::Object,
			"Production snapshot metadata path or kind is incorrect");
		EngineSnapshotObject packageRecord;
		Require(
			source.TryCaptureObject(5, packageRecord) == SnapshotSlotReadResult::Captured
				&& packageRecord.Name == "Fixture"
				&& packageRecord.FullPath == "/Script/Fixture"
				&& packageRecord.ClassPath == "/Script/CoreUObject.Package"
				&& packageRecord.Kind == EngineObjectKind::Package,
			"Production snapshot package metadata is incorrect");

		Require(
			facade.ConfigureSnapshotCapture(source)
				&& facade.SnapshotCapture()->RequestCapture().Ok()
				&& facade.SnapshotCapture()->Pump(4096) == SnapshotPumpResult::Published,
			"Production snapshot source did not publish through the bounded producer");
		const std::shared_ptr<const EngineSnapshot> published = facade.Snapshots().Current();
		Require(
			published && published->SourceObjectCount == 7
				&& published->Objects.size() == 6
				&& published->SkippedSlots == 1,
			"Production snapshot publication lost live or empty slots");

		writeValue(hero, 0x18, actorName);
		Require(
			!source.ValidateSlot(6, &heroRecord),
			"Production snapshot validation ignored a name mutation");
		writeValue(hero, 0x18, heroName);
		identitySource.Objects.at(6).SerialNumber++;
		Require(
			!source.ValidateSlot(6, &heroRecord),
			"Production snapshot validation ignored slot recycling");
		identitySource.Objects.at(6).SerialNumber--;
		writeValue(hero, 0x20, addressOf(hero));
		EngineSnapshotObject cyclic;
		Require(
			source.TryCaptureObject(6, cyclic) == SnapshotSlotReadResult::Failed,
			"Production snapshot metadata source accepted an outer cycle");
		writeValue(hero, 0x20, addressOf(fixturePackage));
		identitySource.ExecutionThreadValid = false;
		Require(
			!source.IsCurrentExecutionThreadValid(),
			"Production snapshot source ignored its execution-thread boundary");
		identitySource.ExecutionThreadValid = true;
		Require(facade.Stop(), "Production snapshot source did not drain from its facade");
	}

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
		source.ExecutionThreadValid = false;
		Require(
			service.ValidateObject(issued.Value).Error == HandleError::ExecutionThreadInvalid,
			"Object handle validation ignored the execution-thread boundary");
		source.ExecutionThreadValid = true;
		source.Generation = 41;
		Require(!service.IsConfigured(), "Handle service ignored an identity-source generation change");
		source.Generation = 42;

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

	void TestEngineFacadeAndImmutableSnapshots()
	{
		using namespace UExplorer::Runtime;

		auto makeRecord = [](const std::string& sessionId,
			const std::uint64_t contextGeneration,
			const std::int32_t index,
			const EngineObjectKind kind) {
			return EngineSnapshotObject{
				.Handle = {
					.SessionId = sessionId,
					.ContextGeneration = contextGeneration,
					.Index = index,
					.SerialNumber = 100 + index,
					.Address = static_cast<std::uintptr_t>(0x1000 + index * 0x100),
					.ClassFingerprint = static_cast<std::uint64_t>(0xA000 + index)
				},
				.Name = "Object" + std::to_string(index),
				.FullPath = "/Script/Fixture.Object" + std::to_string(index),
				.ClassPath = "/Script/CoreUObject.Object",
				.PackagePath = "/Script/Fixture",
				.Kind = kind
			};
		};
		auto makeSnapshot = [&](const std::uint64_t generation) {
			EngineSnapshot snapshot{
				.SessionId = "fixture-snapshot-session",
				.ContextGeneration = 42,
				.Generation = generation,
				.CapturedAtMonotonicUs = 1'000 + generation,
				.CaptureDurationUs = 50,
				.SourceObjectCount = 5,
				.SkippedSlots = 3
			};
			snapshot.Objects.push_back(makeRecord(
				snapshot.SessionId,
				snapshot.ContextGeneration,
				1,
				EngineObjectKind::Class));
			snapshot.Objects.push_back(makeRecord(
				snapshot.SessionId,
				snapshot.ContextGeneration,
				4,
				EngineObjectKind::Function));
			return snapshot;
		};

		EngineSnapshotStore store("fixture-snapshot-session", 42);
		const SnapshotPublishResult first = store.Publish(makeSnapshot(1));
		Require(first.Ok(), "A complete immutable engine snapshot was not published");
		Require(
			first.Snapshot && first.Snapshot->Generation == 1
				&& first.Snapshot->Objects.size() == 2
				&& store.Current() == first.Snapshot,
			"Snapshot publication was not atomic or immutable");

		EngineSnapshot wrongSession = makeSnapshot(2);
		wrongSession.SessionId = "stale-session";
		Require(
			store.Publish(std::move(wrongSession)).Error == SnapshotPublishError::EnvelopeInvalid,
			"Snapshot crossed a Core session boundary");
		EngineSnapshot incomplete = makeSnapshot(2);
		incomplete.Objects[0].ClassPath.clear();
		Require(
			store.Publish(std::move(incomplete)).Error == SnapshotPublishError::RecordInvalid,
			"Incomplete snapshot metadata was published as usable data");
		EngineSnapshot unordered = makeSnapshot(2);
		std::swap(unordered.Objects[0], unordered.Objects[1]);
		Require(
			store.Publish(std::move(unordered)).Error == SnapshotPublishError::RecordsNotOrdered,
			"Unordered snapshot records were published");
		Require(
			store.Publish(makeSnapshot(1)).Error == SnapshotPublishError::GenerationNotMonotonic,
			"A stale snapshot generation replaced the current view");
		Require(store.CurrentGeneration() == 1, "Rejected snapshots changed the published generation");

		std::atomic<bool> keepReading{true};
		std::atomic<bool> tornRead{false};
		auto reader = std::async(std::launch::async, [&] {
			while (keepReading.load(std::memory_order_acquire))
			{
				const std::shared_ptr<const EngineSnapshot> current = store.Current();
				if (!current
					|| current->SessionId != "fixture-snapshot-session"
					|| current->ContextGeneration != 42
					|| current->Objects.size() + current->SkippedSlots
						!= static_cast<std::size_t>(current->SourceObjectCount))
				{
					tornRead.store(true, std::memory_order_release);
					break;
				}
				for (const EngineSnapshotObject& object : current->Objects)
				{
					if (object.Handle.SessionId != current->SessionId
						|| object.Handle.ContextGeneration != current->ContextGeneration)
					{
						tornRead.store(true, std::memory_order_release);
						break;
					}
				}
			}
		});
		for (std::uint64_t generation = 2; generation <= 32; ++generation)
			Require(store.Publish(makeSnapshot(generation)).Ok(), "A newer snapshot generation was rejected");
		keepReading.store(false, std::memory_order_release);
		reader.get();
		Require(!tornRead.load(std::memory_order_acquire), "Snapshot reader observed a torn generation");

		FakeHandleIdentitySource source;
		source.Objects.emplace(7, ObjectIdentity{
			.Index = 7,
			.SerialNumber = 101,
			.Address = 0x1000,
			.ClassFingerprint = 0xA001
		});
		const auto context = MakeEngineContext(42);
		EngineFacade facade(context, "fixture-snapshot-session", source);
		Require(facade.IsConfigured(), "EngineFacade rejected a matching immutable generation");
		Require(facade.Names().IsConfigured(), "EngineFacade did not own its immutable name codec");
		Require(facade.IssueObjectHandle(7).Ok(), "EngineFacade bypassed or lost handle issuance");
		Require(
			facade.Snapshots().Publish(makeSnapshot(1)).Ok()
				&& facade.Snapshots().CurrentGeneration() == 1,
			"EngineFacade did not own its snapshot store");
		Require(facade.Stop(), "EngineFacade snapshot store did not stop");
		Require(
			!facade.IsConfigured()
				&& facade.Snapshots().Publish(makeSnapshot(2)).Error == SnapshotPublishError::StoreStopped,
			"Stopped EngineFacade accepted a new snapshot");

		store.Stop();
		Require(
			store.Publish(makeSnapshot(33)).Error == SnapshotPublishError::StoreStopped,
			"Stopped snapshot store accepted a publisher");
	}

	void TestIncrementalSnapshotCapture()
	{
		using namespace UExplorer::Runtime;
		static_assert(std::is_same_v<
			decltype(EngineSnapshot{}.Objects),
			std::deque<EngineSnapshotObject>>);
		static_assert(!std::is_default_constructible_v<ValidatedEngineSnapshot>);

		FakeSnapshotSource source;
		source.Slots.resize(10);
		for (std::int32_t index = 0; index < 10; index += 2)
		{
			source.Slots[static_cast<std::size_t>(index)] = MakeSnapshotObject(
				"fixture-capture-session",
				42,
				index,
				index == 2 ? EngineObjectKind::Class : EngineObjectKind::Object);
		}
		EngineSnapshotStore store("fixture-capture-session", 42);
		EngineSnapshotCapture capture("fixture-capture-session", 42, source, store);
		Require(capture.IsConfigured(), "Incremental snapshot capture was not configured");
		const SnapshotCaptureRequestResult requested = capture.RequestCapture();
		Require(requested.Ok() && requested.Generation == 1, "Snapshot capture request was rejected");
		Require(
			capture.RequestCapture().Error == SnapshotCaptureError::Busy,
			"Snapshot capture accepted concurrent generation construction");

		SnapshotPumpResult result = SnapshotPumpResult::Idle;
		std::size_t pumpCount = 0;
		while (result != SnapshotPumpResult::Published && pumpCount < 16)
		{
			const std::uint32_t callsBefore = source.WorkCalls.load(std::memory_order_acquire);
			result = capture.Pump(3);
			const std::uint32_t callsAfter = source.WorkCalls.load(std::memory_order_acquire);
			Require(callsAfter - callsBefore <= 3, "Snapshot pump exceeded its per-frame work budget");
			++pumpCount;
		}
		Require(result == SnapshotPumpResult::Published, "Budgeted snapshot capture did not publish");
		Require(pumpCount >= 7, "Snapshot capture traversed capture/validation in one unbounded frame");
		const std::shared_ptr<const EngineSnapshot> first = store.Current();
		Require(
			first && first->Generation == 1 && first->Objects.size() == 5
				&& first->SkippedSlots == 5,
			"Incremental snapshot published incomplete slot accounting");
		const SnapshotCaptureDiagnostics completed = capture.Diagnostics();
		Require(
			completed.State == SnapshotCaptureState::Completed
				&& completed.CapturedObjects == 5
				&& completed.SkippedSlots == 5,
			"Snapshot capture diagnostics did not report the completed generation");

		Require(capture.RequestCapture().Ok(), "Second snapshot generation was not requested");
		Require(
			capture.Pump(10) == SnapshotPumpResult::Progress,
			"Capture phase did not yield before full-slot validation");
		source.Slots[2]->Name = "ChangedDuringCapture";
		Require(
			capture.Pump(10) == SnapshotPumpResult::Failed,
			"A slot mutation between capture and validation was published");
		const SnapshotCaptureDiagnostics stale = capture.Diagnostics();
		Require(
			stale.State == SnapshotCaptureState::Failed
				&& stale.Error == SnapshotCaptureError::SourceValidationFailed
				&& stale.ErrorIndex == 2
				&& store.CurrentGeneration() == 1,
			"Failed revalidation replaced the last complete snapshot");
		source.Slots[2]->Name = "Object2";

		Require(capture.RequestCapture().Ok(), "Object-count mutation capture was not requested");
		Require(
			capture.Pump(11) == SnapshotPumpResult::Progress,
			"Object-count mutation fixture did not finish its capture phase");
		source.Slots.resize(11);
		Require(
			capture.Pump(11) == SnapshotPumpResult::Failed
				&& capture.Diagnostics().Error == SnapshotCaptureError::SourceCountChanged
				&& store.CurrentGeneration() == 1,
			"Object-count mutation was published as a complete snapshot");
		source.Slots.resize(10);
		Require(capture.RequestCapture().Ok(), "Publication-count mutation capture was not requested");
		Require(
			capture.Pump(21) == SnapshotPumpResult::Progress
				&& capture.Diagnostics().State == SnapshotCaptureState::Publishing,
			"Publication-count mutation fixture did not reach its publishing phase");
		source.Slots.resize(11);
		Require(
			capture.Pump(1) == SnapshotPumpResult::Failed
				&& capture.Diagnostics().Error == SnapshotCaptureError::SourceCountChanged
				&& store.CurrentGeneration() == 1,
			"Object-count mutation during publication replaced the complete snapshot");
		source.Slots.resize(10);

		Require(capture.RequestCapture().Ok(), "Execution-thread snapshot generation was not requested");
		source.ExecutionThreadValid.store(false, std::memory_order_release);
		Require(
			capture.Pump(1) == SnapshotPumpResult::Failed
				&& capture.Diagnostics().Error == SnapshotCaptureError::ExecutionThreadInvalid,
			"Snapshot capture ran outside its verified execution thread");
		source.ExecutionThreadValid.store(true, std::memory_order_release);
		Require(
			capture.Pump(0) == SnapshotPumpResult::InvalidBudget
				&& capture.Pump(EngineSnapshotCapture::kMaxPumpBudget + 1)
					== SnapshotPumpResult::InvalidBudget,
			"Snapshot capture accepted an invalid per-frame budget");
		Require(capture.RequestCapture().Ok(), "Exception capture generation was not requested");
		source.ThrowOnCount.store(true, std::memory_order_release);
		Require(
			capture.Pump(1) == SnapshotPumpResult::Failed
				&& capture.Diagnostics().Error == SnapshotCaptureError::UnexpectedException,
			"Snapshot source exception escaped the guarded pump boundary");
		source.ThrowOnCount.store(false, std::memory_order_release);
		Require(capture.RequestCapture().Ok(), "Generation-drift capture was not requested");
		Require(capture.Pump(1) == SnapshotPumpResult::Progress, "Generation-drift fixture did not start");
		source.Generation = 43;
		Require(
			capture.Pump(1) == SnapshotPumpResult::Failed
				&& capture.Diagnostics().Error == SnapshotCaptureError::SourceContextMismatch,
			"Snapshot producer crossed an identity-source context generation");
		source.Generation = 42;
		const std::string classPath = source.Slots[2]->ClassPath;
		source.Slots[2]->ClassPath.clear();
		Require(capture.RequestCapture().Ok(), "Invalid-record capture was not requested");
		Require(
			capture.Pump(4) == SnapshotPumpResult::Failed
				&& capture.Diagnostics().Error == SnapshotCaptureError::PublicationRejected
				&& capture.Diagnostics().ErrorIndex == 2,
			"Snapshot producer deferred malformed-record validation to the final publication frame");
		source.Slots[2]->ClassPath = classPath;
		Require(
			capture.Diagnostics().RetiredCaptures >= 5
				&& capture.ReclaimRetired() >= 5
				&& capture.Diagnostics().RetiredCaptures == 0,
			"Failed snapshot working sets were destroyed on the frame thread instead of retired");
		Require(
			capture.RequestCapture().Ok()
				&& capture.Pump(64) == SnapshotPumpResult::Published
				&& store.RetiredSnapshotCount() == 1
				&& store.ReclaimRetired() == 1
				&& store.RetiredSnapshotCount() == 0,
			"Atomic snapshot replacement did not defer prior-generation reclamation");

		EngineSnapshotStore blockingStore("fixture-capture-session", 42);
		EngineSnapshotCapture blockingCapture(
			"fixture-capture-session",
			42,
			source,
			blockingStore);
		Require(blockingCapture.RequestCapture().Ok(), "Blocking capture fixture did not start");
		source.CaptureEntered.store(false, std::memory_order_release);
		source.BlockCapture.store(true, std::memory_order_release);
		auto pumping = std::async(std::launch::async, [&blockingCapture] {
			return blockingCapture.Pump(2);
		});
		WaitUntil(
			[&source] { return source.CaptureEntered.load(std::memory_order_acquire); },
			"Blocking snapshot source was not entered");
		Require(
			blockingCapture.Pump(1) == SnapshotPumpResult::Busy,
			"Concurrent snapshot pumps accessed one mutable working generation");
		Require(
			!blockingCapture.StopAndDrain(std::chrono::milliseconds(1)),
			"Snapshot shutdown ignored an in-flight pump");
		source.BlockCapture.store(false, std::memory_order_release);
		Require(
			pumping.get() == SnapshotPumpResult::Stopping,
			"In-flight snapshot pump published after shutdown began");
		Require(
			blockingCapture.StopAndDrain(std::chrono::milliseconds(100))
				&& blockingCapture.Diagnostics().State == SnapshotCaptureState::Stopped
				&& blockingCapture.RequestCapture().Error == SnapshotCaptureError::Stopped,
			"Snapshot capture did not drain to a terminal stopped state");
		Require(
			capture.StopAndDrain(std::chrono::milliseconds(100)),
			"Completed/failed snapshot capture did not stop cleanly");

		FakeSnapshotSource retirementSource;
		retirementSource.Slots.resize(1);
		retirementSource.Slots[0] = MakeSnapshotObject(
			"fixture-retirement-session",
			99,
			0,
			EngineObjectKind::Object);
		retirementSource.Generation = 99;
		retirementSource.FailReadIndex.store(0, std::memory_order_release);
		EngineSnapshotStore retirementStore("fixture-retirement-session", 99);
		EngineSnapshotCapture retirementCapture(
			"fixture-retirement-session",
			99,
			retirementSource,
			retirementStore);
		for (std::size_t index = 0; index < EngineSnapshotCapture::kMaxRetiredCaptures; ++index)
		{
			Require(
				retirementCapture.RequestCapture().Ok()
					&& retirementCapture.Pump(2) == SnapshotPumpResult::Failed,
				"Retirement-capacity fixture did not produce a failed working set");
		}
		Require(
			retirementCapture.RequestCapture().Ok()
				&& retirementCapture.Pump(2) == SnapshotPumpResult::Failed
				&& retirementCapture.RequestCapture().Error
					== SnapshotCaptureError::RetirementBackpressure
				&& retirementCapture.Diagnostics().RetiredCaptures
					== EngineSnapshotCapture::kMaxRetiredCaptures,
			"Snapshot failure retirement exceeded its fixed capacity without backpressure");
		Require(
			retirementCapture.ReclaimRetired()
				== EngineSnapshotCapture::kMaxRetiredCaptures + 1,
			"Snapshot retirement did not reclaim the capacity-stalled working set off-frame");
		retirementSource.FailReadIndex.store(-1, std::memory_order_release);
		Require(
			retirementCapture.RequestCapture().Ok()
				&& retirementCapture.Pump(4) == SnapshotPumpResult::Published,
			"Snapshot retirement backpressure did not clear after explicit reclamation");
		Require(retirementCapture.StopAndDrain(), "Retirement capture did not stop");

		FakeSnapshotSource publicationSource;
		publicationSource.Slots.resize(1);
		publicationSource.Slots[0] = MakeSnapshotObject(
			"fixture-publication-session",
			100,
			0,
			EngineObjectKind::Object);
		publicationSource.Generation = 100;
		EngineSnapshotStore publicationStore("fixture-publication-session", 100);
		EngineSnapshotCapture publicationCapture(
			"fixture-publication-session",
			100,
			publicationSource,
			publicationStore);
		for (std::size_t generation = 0;
			generation <= EngineSnapshotStore::kMaxRetiredSnapshots;
			++generation)
		{
			Require(
				publicationCapture.RequestCapture().Ok()
					&& publicationCapture.Pump(4) == SnapshotPumpResult::Published,
				"Validated publication retirement fixture rejected an in-capacity generation");
		}
		Require(
			publicationStore.RetiredSnapshotCount()
				== EngineSnapshotStore::kMaxRetiredSnapshots,
			"Validated publication did not retain every replaced generation");
		Require(
			publicationCapture.RequestCapture().Ok()
				&& publicationCapture.Pump(4) == SnapshotPumpResult::Failed
				&& publicationCapture.Diagnostics().Error
					== SnapshotCaptureError::RetirementBackpressure
				&& publicationCapture.RequestCapture().Error
					== SnapshotCaptureError::RetirementBackpressure
				&& publicationStore.RetiredSnapshotCount()
					== EngineSnapshotStore::kMaxRetiredSnapshots + 1,
			"Validated publication exceeded retirement capacity without explicit backpressure");
		Require(
			publicationStore.ReclaimRetired()
				== EngineSnapshotStore::kMaxRetiredSnapshots + 1
				&& publicationCapture.ReclaimRetired() == 1,
			"Publication backpressure did not preserve rejected ownership for worker reclamation");
		Require(
			publicationCapture.RequestCapture().Ok()
				&& publicationCapture.Pump(4) == SnapshotPumpResult::Published,
			"Validated publication did not resume after worker reclamation");
		Require(publicationCapture.StopAndDrain(), "Publication retirement capture did not stop");

		FakeHandleIdentitySource identitySource;
		EngineFacade owner(
			MakeEngineContext(42),
			"fixture-capture-session",
			identitySource);
		Require(
			owner.ConfigureSnapshotCapture(source)
				&& !owner.ConfigureSnapshotCapture(source)
				&& owner.SnapshotCapture(),
			"EngineFacade did not uniquely own its snapshot producer");
		Require(
			owner.SnapshotCapture()->RequestCapture().Ok()
				&& owner.SnapshotCapture()->Pump(64) == SnapshotPumpResult::Published
				&& owner.Snapshots().CurrentGeneration() == 1,
			"EngineFacade-owned snapshot producer did not publish through its store");
		Require(owner.Stop(), "EngineFacade did not drain its owned snapshot producer");
	}

	void TestCoreDomainCommandsAndHandleExecution()
	{
		using namespace UExplorer::Runtime;
		using namespace UExplorer::Services;

		CoreRuntime runtime;
		Require(runtime.BeginInitialize("fixture-command-session"), "Command runtime did not initialize");
		const auto context = MakeEngineContext(77);
		Require(runtime.PublishContext(context), "Command runtime rejected its EngineContext");
		RuntimeProbes probes;
		probes.GameThreadExecutorEnabled = true;
		probes.GameThreadPumpObserved = true;
		probes.GameThreadPumpThreadStable = true;
		probes.GameThreadPumpActive = true;
		probes.SafeMemoryEnabled = true;
		probes.ObjectIdentitySourceEnabled = true;
		probes.ObjectHandleValidationEnabled = true;
		probes.FunctionHandleValidationEnabled = true;
		probes.NamedPipeListening = true;
		Require(
			runtime.PublishCapabilities(BuildCoreCapabilities(*context, probes))
				&& runtime.TryMarkReady(RequiredReadyCapabilities()),
			"Command runtime did not become ready");

		GameThreadExecutor executor;
		Require(executor.Enable(&FakeProcessEvent), "Command executor did not enable");
		FakeHandleIdentitySource source;
		source.Generation = 77;
		source.Objects.emplace(7, ObjectIdentity{
			.Index = 7,
			.SerialNumber = 101,
			.Address = 0x1000,
			.ClassFingerprint = 0xA001
		});
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
		FakeCoreStatusDiagnostics diagnostics;
		EngineFacade engine(context, "fixture-command-session", source);
		ObjectSnapshotReflectionCandidateSource reflectionSource(context, engine);
		CoreCommandService service(
			runtime,
			executor,
			engine,
			diagnostics,
			&reflectionSource);
		Require(service.IsConfigured(), "Core domain command service was not configured");

		const CoreCommandResponse status = service.Execute({
			.RequestId = 1,
			.Operation = "status.inspect",
			.SessionId = service.SessionId(),
			.TimeoutMs = 5000,
			.Data = json::object()
		});
		Require(
			status.Ok
				&& status.Data.at("runtime").at("session_id") == "fixture-command-session"
				&& status.Data.at("architecture") == "x64-fixture"
				&& status.Data.at("name_profile").at("validated").get<bool>()
				&& status.Data.at("name_profile").at("storage") == "name_pool"
				&& !status.Data.at("object_snapshot").at("published").get<bool>()
				&& !status.Data.at("object_snapshot").at("capture_configured").get<bool>()
				&& status.Data.at("object_snapshot").at("retired_snapshot_count") == 0
				&& !status.Data.at("reflection").at("layout_published").get<bool>()
				&& !status.Data.at("reflection").at("capture_configured").get<bool>()
				&& status.Data.at("reflection").at("source").at("property_system")
					== "unavailable"
				&& !status.Data.at("type_snapshot").at("published").get<bool>()
				&& !status.Data.at("type_snapshot").at("capture_configured").get<bool>(),
			"Status domain command did not serialize the immutable runtime/name profile");

		const CoreCommandResponse unavailableType = service.Execute({
			.RequestId = 16,
			.Operation = "types.classes.get",
			.SessionId = service.SessionId(),
			.TimeoutMs = 1000,
			.Data = {{"path", "/Script/Fixture.Missing"}}
		});
		Require(
			!unavailableType.Ok
				&& unavailableType.Error
				&& unavailableType.Error->Code != "OPERATION_NOT_SUPPORTED"
				&& unavailableType.Error->Details.at("capability") == "types.inspect",
			"Core command registry did not route type operations through their capability boundary");

		CoreCommandRequest objectRequest{
			.RequestId = 2,
			.Operation = "objects.handle.issue",
			.SessionId = service.SessionId(),
			.TimeoutMs = 1000,
			.Data = {{"index", 7}}
		};
		auto objectFuture = std::async(std::launch::async, [&service, objectRequest] {
			return service.Execute(objectRequest);
		});
		WaitUntil([&executor] { return executor.HasPending(); }, "Object handle command was not queued");
		executor.Pump();
		const CoreCommandResponse objectResponse = objectFuture.get();
		Require(
			objectResponse.Ok
				&& objectResponse.Data.at("session_id") == "fixture-command-session"
				&& objectResponse.Data.at("context_generation") == 77
				&& objectResponse.Data.at("serial") == 101
				&& objectResponse.Data.at("address") == "0x1000",
			"Object handle domain command did not return a stable execution identity");

		CoreCommandRequest functionRequest{
			.RequestId = 3,
			.Operation = "functions.handle.issue",
			.SessionId = service.SessionId(),
			.TimeoutMs = 1000,
			.Data = {{"index", 100}}
		};
		auto functionFuture = std::async(std::launch::async, [&service, functionRequest] {
			return service.Execute(functionRequest);
		});
		WaitUntil([&executor] { return executor.HasPending(); }, "Function handle command was not queued");
		executor.Pump();
		const CoreCommandResponse functionResponse = functionFuture.get();
		Require(
			functionResponse.Ok
				&& functionResponse.Data.at("full_path")
					== "Function fname:10:0.fname:20:0"
				&& functionResponse.Data.at("owner").at("serial") == 401,
			"Function handle domain command did not bind its owner/path identity");

		CoreCommandRequest pageRequest{
			.RequestId = 4,
			.Operation = "objects.snapshot.page",
			.SessionId = service.SessionId(),
			.TimeoutMs = 1000,
			.Data = {{"cursor", nullptr}, {"limit", 128}}
		};
		const CoreCommandResponse unavailablePage = service.Execute(pageRequest);
		Require(
			!unavailablePage.Ok && unavailablePage.Error
				&& unavailablePage.Error->Code == "OBJECT_SNAPSHOT_UNAVAILABLE",
			"Snapshot page command ignored its publication capability");

		const auto makeSnapshot = [](const std::uint64_t generation) {
			EngineSnapshot snapshot{
				.SessionId = "fixture-command-session",
				.ContextGeneration = 77,
				.Generation = generation,
				.CapturedAtMonotonicUs = 1'000'000 + generation,
				.CaptureDurationUs = 2'500,
				.SourceObjectCount = 300,
				.SkippedSlots = 40
			};
			for (std::int32_t index = 0; index < 260; ++index)
			{
				const std::string suffix = std::to_string(index);
				EngineSnapshotObject record;
				record.Handle = {
					.SessionId = "fixture-command-session",
					.ContextGeneration = 77,
					.Index = index,
					.SerialNumber = index + 1,
					.Address = static_cast<std::uintptr_t>(0x100000)
						+ static_cast<std::uintptr_t>(index) * 0x100,
					.ClassFingerprint = 0xA000ULL + static_cast<std::uint64_t>(index)
				};
				record.Name = "FixtureObject" + suffix;
				record.FullPath = "Object /Game/Fixture.FixtureObject" + suffix;
				record.ClassPath = "Class /Script/CoreUObject.Object";
				record.PackagePath = "Package /Game/Fixture";
				record.Kind = index == 1 ? EngineObjectKind::Class : EngineObjectKind::Object;
				snapshot.Objects.push_back(std::move(record));
			}
			return snapshot;
		};
		Require(
			engine.Snapshots().Publish(makeSnapshot(1)).Ok(),
			"Snapshot page fixture generation did not publish");
		probes.ObjectSnapshotPublished = true;
		Require(
			runtime.PublishCapabilities(BuildCoreCapabilities(*context, probes)),
			"Command runtime rejected the published snapshot capability");

		pageRequest.RequestId = 5;
		const CoreCommandResponse firstPage = service.Execute(pageRequest);
		Require(
			firstPage.Ok
				&& firstPage.Timing.QueuedUs == 0
				&& firstPage.Data.at("generation") == 1
				&& firstPage.Data.at("context_generation") == 77
				&& firstPage.Data.at("source_object_count") == 300
				&& firstPage.Data.at("record_count") == 260
				&& firstPage.Data.at("skipped_slots") == 40
				&& firstPage.Data.at("items").size() == 128
				&& firstPage.Data.at("items").front().at("handle").at("index") == 0
				&& firstPage.Data.at("items").back().at("handle").at("index") == 127
				&& firstPage.Data.at("has_more").get<bool>()
				&& firstPage.Data.at("next_cursor").at("generation") == 1
				&& firstPage.Data.at("next_cursor").at("after_index") == 127,
			"Snapshot first page did not preserve immutable generation and exact totals");

		pageRequest.RequestId = 6;
		pageRequest.Data = {
			{"cursor", firstPage.Data.at("next_cursor")},
			{"limit", 128}
		};
		const CoreCommandResponse secondPage = service.Execute(pageRequest);
		Require(
			secondPage.Ok
				&& secondPage.Data.at("items").size() == 128
				&& secondPage.Data.at("items").front().at("handle").at("index") == 128
				&& secondPage.Data.at("items").back().at("handle").at("index") == 255
				&& secondPage.Data.at("has_more").get<bool>()
				&& secondPage.Data.at("next_cursor").at("after_index") == 255,
			"Snapshot continuation page skipped or repeated an object index");

		pageRequest.RequestId = 7;
		pageRequest.Data = {
			{"cursor", secondPage.Data.at("next_cursor")},
			{"limit", 128}
		};
		const CoreCommandResponse finalPage = service.Execute(pageRequest);
		Require(
			finalPage.Ok
				&& finalPage.Data.at("items").size() == 4
				&& finalPage.Data.at("items").front().at("handle").at("index") == 256
				&& finalPage.Data.at("items").back().at("handle").at("index") == 259
				&& !finalPage.Data.at("has_more").get<bool>()
				&& finalPage.Data.at("next_cursor").is_null(),
			"Snapshot final page did not terminate its generation cursor");

		pageRequest.RequestId = 8;
		pageRequest.Data = {{"cursor", nullptr}, {"limit", 129}};
		const CoreCommandResponse rejectedPageLimit = service.Execute(pageRequest);
		Require(
			!rejectedPageLimit.Ok && rejectedPageLimit.Error
				&& rejectedPageLimit.Error->Code == "INVALID_ARGUMENT",
			"Snapshot page command accepted an out-of-range limit");
		pageRequest.RequestId = 9;
		pageRequest.Data = {{"cursor", nullptr}, {"limit", 128}, {"query", "Object"}};
		const CoreCommandResponse rejectedPageFilter = service.Execute(pageRequest);
		Require(
			!rejectedPageFilter.Ok && rejectedPageFilter.Error
				&& rejectedPageFilter.Error->Code == "INVALID_ARGUMENT",
			"Snapshot page command accepted a Core-side search filter");

		Require(
			engine.Snapshots().Publish(makeSnapshot(2)).Ok(),
			"New snapshot generation did not publish");
		pageRequest.RequestId = 10;
		pageRequest.Data = {
			{"cursor", firstPage.Data.at("next_cursor")},
			{"limit", 128}
		};
		const CoreCommandResponse stalePage = service.Execute(pageRequest);
		Require(
			!stalePage.Ok && stalePage.Error
				&& stalePage.Error->Code == "SNAPSHOT_GENERATION_MISMATCH"
				&& stalePage.Error->Details.at("requested_generation") == 1
				&& stalePage.Error->Details.at("current_generation") == 2,
			"Snapshot cursor silently crossed an immutable generation boundary");
		Require(
			!executor.HasPending(),
			"Worker-safe snapshot paging entered the game-thread command queue");

		RuntimeProbes objectOnlyProbes = probes;
		objectOnlyProbes.FunctionHandleValidationEnabled = false;
		Require(
			runtime.PublishCapabilities(BuildCoreCapabilities(*context, objectOnlyProbes)),
			"Command runtime rejected a truthful function-handle capability downgrade");
		functionRequest.RequestId = 11;
		const CoreCommandResponse unavailableFunction = service.Execute(functionRequest);
		Require(
			!unavailableFunction.Ok && unavailableFunction.Error
				&& unavailableFunction.Error->Code == "FUNCTION_HANDLE_VALIDATION_NOT_READY",
			"Function handle command ignored its dedicated capability");

		CoreCommandRequest wrongSession = objectRequest;
		wrongSession.RequestId = 12;
		wrongSession.SessionId = "stale-session";
		const CoreCommandResponse rejectedSession = service.Execute(wrongSession);
		Require(
			!rejectedSession.Ok && rejectedSession.Error
				&& rejectedSession.Error->Code == "SESSION_MISMATCH",
			"Domain command crossed a Core session boundary");

		CoreCommandRequest invalidData = objectRequest;
		invalidData.RequestId = 13;
		invalidData.Data = {{"index", 7}, {"address", "0x1000"}};
		const CoreCommandResponse rejectedData = service.Execute(invalidData);
		Require(
			!rejectedData.Ok && rejectedData.Error
				&& rejectedData.Error->Code == "INVALID_ARGUMENT",
			"Handle command accepted transport-supplied identity fields");
		invalidData.RequestId = 14;
		invalidData.Data = {{"index", (std::numeric_limits<std::uint64_t>::max)()}};
		const CoreCommandResponse rejectedUnsignedIndex = service.Execute(invalidData);
		Require(
			!rejectedUnsignedIndex.Ok && rejectedUnsignedIndex.Error
				&& rejectedUnsignedIndex.Error->Code == "INVALID_ARGUMENT",
			"Handle command narrowed an out-of-range unsigned index");

		std::promise<GameThreadTicket> publishedTicket;
		auto ticketFuture = publishedTicket.get_future();
		CoreCommandRequest cancelledRequest = objectRequest;
		cancelledRequest.RequestId = 15;
		auto cancelledResponseFuture = std::async(
			std::launch::async,
			[&service, cancelledRequest, &publishedTicket] {
				return service.Execute(
					cancelledRequest,
					[&publishedTicket](const GameThreadTicket& ticket) {
						publishedTicket.set_value(ticket);
					});
			});
		Require(
			ticketFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
			"Domain command did not publish its cancellation ticket");
		const GameThreadTicket ticket = ticketFuture.get();
		Require(
			executor.Cancel(ticket) == GameThreadCancelResult::Cancelled,
			"Domain command ticket did not cancel queued work");
		const CoreCommandResponse cancelledResponse = cancelledResponseFuture.get();
		Require(
			!cancelledResponse.Ok && cancelledResponse.Error
				&& cancelledResponse.Error->Code == "REQUEST_CANCELLED",
			"Cancelled domain command did not return its explicit terminal error");

		Require(runtime.BeginStopping(), "Command runtime did not begin stopping");
		Require(executor.DisableAndDrain(), "Command executor did not drain");
		Require(
			runtime.WaitForRequests(std::chrono::milliseconds(100)),
			"Domain command request leases did not drain");
		Require(runtime.MarkStopped(), "Command runtime did not stop");
	}

	void TestNamedPipeRpcServerLifecycle()
	{
		using namespace UExplorer::IPC;
		using namespace UExplorer::Runtime;
		using namespace UExplorer::Services;

		const std::uint32_t processId = GetCurrentProcessId();
		CoreRuntime runtime;
		Require(runtime.BeginInitialize("fixture-pipe-session"), "Pipe runtime did not initialize");
		const auto context = MakeEngineContext(88, true, true, nullptr, processId);
		Require(runtime.PublishContext(context), "Pipe runtime rejected its EngineContext");

		GameThreadExecutor executor;
		Require(executor.Enable(&FakeProcessEvent), "Pipe game-thread executor did not enable");
		FakeHandleIdentitySource source;
		source.Generation = 88;
		source.Objects.emplace(7, ObjectIdentity{
			.Index = 7,
			.SerialNumber = 501,
			.Address = 0x7100,
			.ClassFingerprint = 0xCAFE
		});
		EngineFacade engine(context, "fixture-pipe-session", source);
		FakeCoreStatusDiagnostics diagnostics;
		CoreCommandService service(runtime, executor, engine, diagnostics);
		Require(service.IsConfigured(), "Pipe command service was not configured");

		std::atomic<bool> shutdownObserved{false};
		NamedPipeRpcServer server(
			runtime,
			service,
			executor,
			[&shutdownObserved] { shutdownObserved.store(true, std::memory_order_release); });
		Require(server.Start(), "Real Windows named-pipe server did not bind");
		Require(
			!server.OpenAdmissions(),
			"Named-pipe server admitted a handshake before CoreRuntime was Ready");
		Require(
			server.PublishEvent("fixture.pipe", 1, {{"phase", "pre_ready"}})
				== EventPublishResult::Unavailable,
			"Named-pipe event admission opened before CoreRuntime was Ready");
		Require(
			server.PipeName() == L"\\\\.\\pipe\\UExplorer\\v1\\" + std::to_wstring(processId)
				&& server.IsListening(),
			"Named-pipe server did not expose the canonical target-PID endpoint");

		RuntimeProbes probes;
		probes.GameThreadExecutorEnabled = true;
		probes.GameThreadPumpObserved = true;
		probes.GameThreadPumpThreadStable = true;
		probes.GameThreadPumpActive = true;
		probes.SafeMemoryEnabled = true;
		probes.ObjectIdentitySourceEnabled = true;
		probes.ObjectHandleValidationEnabled = true;
		probes.FunctionHandleValidationEnabled = true;
		probes.NamedPipeListening = server.IsListening();
		Require(
			runtime.PublishCapabilities(BuildCoreCapabilities(*context, probes))
				&& runtime.TryMarkReady(RequiredReadyCapabilities())
				&& server.OpenAdmissions(),
			"Pipe admissions opened before the runtime readiness contract was satisfied");

		auto connectFixtureClient = [&server] {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (std::chrono::steady_clock::now() < deadline)
			{
				if (WaitNamedPipeW(server.PipeName().c_str(), 50) != FALSE)
				{
					const HANDLE pipe = CreateFileW(
						server.PipeName().c_str(),
						GENERIC_READ | GENERIC_WRITE,
						0,
						nullptr,
						OPEN_EXISTING,
						0,
						nullptr);
					if (pipe != INVALID_HANDLE_VALUE)
						return pipe;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			throw std::runtime_error("Named-pipe disconnect-matrix connection deadline expired");
		};
		const nlohmann::json matrixHelloPayload = {
			{"host_version", "core-harness-matrix-0.1.0"},
			{"protocol", {{"major", ProtocolMajor}, {"minor", ProtocolMinor}}},
			{"target_pid", processId}
		};
		const std::string matrixHelloJson = matrixHelloPayload.dump();
		std::vector<std::uint8_t> matrixHello;
		Require(
			EncodeFrame(
				FrameKind::Hello,
				1,
				std::span<const std::uint8_t>(
					reinterpret_cast<const std::uint8_t*>(matrixHelloJson.data()),
					matrixHelloJson.size()),
				matrixHello) == ProtocolError::None,
			"Named-pipe disconnect matrix Hello did not encode");

		std::uint64_t expectedRejected = 0;
		auto rejectConnection = [&](const std::vector<std::uint8_t>& bytes) {
			const HANDLE rejected = connectFixtureClient();
			if (!bytes.empty())
				WritePipeBytes(rejected, bytes);
			CloseHandle(rejected);
			++expectedRejected;
			WaitUntil(
				[&server, expectedRejected] {
					return server.Diagnostics().RejectedConnections >= expectedRejected;
				},
				"Named-pipe disconnect matrix did not retire a rejected connection");
		};
		rejectConnection({});
		rejectConnection(std::vector<std::uint8_t>(
			matrixHello.begin(),
			matrixHello.begin() + static_cast<std::ptrdiff_t>(HeaderSize - 1)));
		rejectConnection(std::vector<std::uint8_t>(matrixHello.begin(), matrixHello.end() - 1));
		using HeaderMutation = void(*)(std::vector<std::uint8_t>&);
		const std::array<HeaderMutation, 5> headerMutations{
			[](std::vector<std::uint8_t>& bytes) { bytes[0] = 'X'; },
			[](std::vector<std::uint8_t>& bytes) { Detail::WriteU16(bytes.data() + 4, ProtocolMajor + 1); },
			[](std::vector<std::uint8_t>& bytes) { Detail::WriteU16(bytes.data() + 8, 0xFFFF); },
			[](std::vector<std::uint8_t>& bytes) { Detail::WriteU16(bytes.data() + 10, 1); },
			[](std::vector<std::uint8_t>& bytes) { Detail::WriteU32(bytes.data() + 12, MaxPayloadSize + 1); }
		};
		for (const HeaderMutation mutateHeader : headerMutations)
		{
			std::vector<std::uint8_t> malformed = matrixHello;
			mutateHeader(malformed);
			rejectConnection(malformed);
		}

		const HANDLE idleClient = connectFixtureClient();
		WritePipeBytes(idleClient, matrixHello);
		const Frame idleWelcome = ReadPipeFrame(idleClient);
		Require(
			idleWelcome.Header.Kind == FrameKind::Welcome,
			"Named-pipe idle-disconnect fixture did not complete its handshake");
		CloseHandle(idleClient);
		WaitUntil(
			[&server] { return server.Diagnostics().AcceptedConnections >= 1; },
			"Named-pipe idle disconnect was not accounted for");

		const HANDLE client = connectFixtureClient();
		ULONG serverProcessId = 0;
		Require(
			GetNamedPipeServerProcessId(client, &serverProcessId) != FALSE
				&& serverProcessId == processId,
			"Named-pipe client did not prove the server PID");

		WriteJsonPipeFrame(client, FrameKind::Hello, 1, {
			{"host_version", "core-harness-0.1.0"},
			{"protocol", {{"major", ProtocolMajor}, {"minor", ProtocolMinor}}},
			{"target_pid", processId}
		});
		Frame welcomeFrame;
		try
		{
			welcomeFrame = ReadPipeFrame(client);
		}
		catch (const std::exception& error)
		{
			const NamedPipeServerDiagnostics state = server.Diagnostics();
			throw std::runtime_error(
				std::string(error.what()) + " server=" + state.LastErrorCode
				+ " native=" + std::to_string(state.LastNativeError)
				+ " message=" + state.LastErrorMessage);
		}
		const nlohmann::json welcome = ParseFrameJson(welcomeFrame);
		Require(
			welcomeFrame.Header.Kind == FrameKind::Welcome
				&& welcomeFrame.Header.RequestId == 1
				&& welcome.at("session_id") == "fixture-pipe-session"
				&& welcome.at("target_pid") == processId
				&& welcome.at("capabilities").at("engine.core").get<bool>()
				&& welcome.at("capabilities").at("transport.named_pipe").get<bool>()
				&& welcome.at("limits").at("pending_rpc_per_session") == 256
				&& welcome.at("limits").at("max_payload_bytes") == MaxPayloadSize,
			"Hello/Welcome did not negotiate the strict shared v1 contract");

		Require(
			server.PublishEvent("fixture.pipe", 1'234'567, {
				{"source", "real-core"},
				{"generation", 88}
			}) == EventPublishResult::Accepted,
			"Named-pipe server rejected a bounded event after handshake");
		const Frame eventFrame = ReadPipeFrame(client);
		const nlohmann::json event = ParseFrameJson(eventFrame);
		Require(
			eventFrame.Header.Kind == FrameKind::Event
				&& eventFrame.Header.RequestId == 0
				&& event.at("seq") == 1
				&& event.at("kind") == "fixture.pipe"
				&& event.at("timestamp_us") == 1'234'567
				&& event.at("session_id") == "fixture-pipe-session"
				&& event.at("dropped_before") == 0
				&& event.at("data").at("source") == "real-core",
			"Named-pipe event writer did not preserve the strict event envelope");

		WriteJsonPipeFrame(client, FrameKind::Request, 2, {
			{"operation", "status.inspect"},
			{"session_id", "fixture-pipe-session"},
			{"timeout_ms", 5000},
			{"data", nlohmann::json::object()}
		});
		const Frame statusFrame = ReadPipeFrame(client);
		const nlohmann::json status = ParseFrameJson(statusFrame);
		Require(
			statusFrame.Header.Kind == FrameKind::Response
				&& statusFrame.Header.RequestId == 2
				&& status.at("ok").get<bool>()
				&& status.at("request_id") == 2
				&& status.at("session_id") == "fixture-pipe-session"
				&& status.at("error").is_null()
				&& status.at("data").at("pid") == processId,
			"Named-pipe request did not reach the Core domain service");

		const nlohmann::json heartbeat = {
			{"session_id", "fixture-pipe-session"},
			{"nonce", 7},
			{"sent_at_monotonic_us", 1'000'000}
		};
		WriteJsonPipeFrame(client, FrameKind::Ping, 3, heartbeat);
		const Frame pongFrame = ReadPipeFrame(client);
		Require(
			pongFrame.Header.Kind == FrameKind::Pong
				&& pongFrame.Header.RequestId == 3
				&& ParseFrameJson(pongFrame) == heartbeat,
			"Named-pipe Ping/Pong lost correlation or heartbeat identity");

		WriteJsonPipeFrame(client, FrameKind::Request, 4, {
			{"operation", "objects.handle.issue"},
			{"session_id", "fixture-pipe-session"},
			{"timeout_ms", 5000},
			{"data", {{"index", 7}}}
		});
		WaitUntil([&executor] { return executor.HasPending(); }, "Pipe request did not enter the game-thread queue");
		WriteJsonPipeFrame(client, FrameKind::Cancel, 4, {
			{"session_id", "fixture-pipe-session"},
			{"reason", "fixture_cancel"}
		});
		const Frame cancelledFrame = ReadPipeFrame(client);
		const nlohmann::json cancelled = ParseFrameJson(cancelledFrame);
		Require(
			cancelledFrame.Header.Kind == FrameKind::Response
				&& cancelledFrame.Header.RequestId == 4
				&& !cancelled.at("ok").get<bool>()
				&& cancelled.at("data").is_null()
				&& cancelled.at("error").at("code") == "REQUEST_CANCELLED",
			"Pipe reader did not remain responsive enough to cancel queued game-thread work");

		const nlohmann::json shutdown = {
			{"session_id", "fixture-pipe-session"},
			{"reason", "host_exit"}
		};
		WriteJsonPipeFrame(client, FrameKind::Shutdown, 5, shutdown);
		const Frame shutdownFrame = ReadPipeFrame(client);
		Require(
			shutdownFrame.Header.Kind == FrameKind::Shutdown
				&& shutdownFrame.Header.RequestId == 5
				&& ParseFrameJson(shutdownFrame) == shutdown,
			"Named-pipe shutdown acknowledgement was not exact");
		WaitUntil(
			[&shutdownObserved] { return shutdownObserved.load(std::memory_order_acquire); },
			"Host-controlled shutdown callback was not invoked");
		CloseHandle(client);

		WaitUntil(
			[&server] { return WaitNamedPipeW(server.PipeName().c_str(), 50) != FALSE; },
			"Named-pipe listener did not return after a graceful Host session ended");
		const HANDLE reconnectClient = CreateFileW(
			server.PipeName().c_str(),
			GENERIC_READ | GENERIC_WRITE,
			0,
			nullptr,
			OPEN_EXISTING,
			0,
			nullptr);
		Require(reconnectClient != INVALID_HANDLE_VALUE, "Named-pipe Host restart could not reconnect");
		WriteJsonPipeFrame(reconnectClient, FrameKind::Hello, 10, {
			{"host_version", "core-harness-reconnect-0.1.0"},
			{"protocol", {{"major", ProtocolMajor}, {"minor", ProtocolMinor}}},
			{"target_pid", processId}
		});
		const Frame reconnectWelcome = ReadPipeFrame(reconnectClient);
		Require(
			reconnectWelcome.Header.Kind == FrameKind::Welcome
				&& reconnectWelcome.Header.RequestId == 10
				&& ParseFrameJson(reconnectWelcome).at("session_id") == "fixture-pipe-session",
			"Reconnected Host did not receive a correlated Welcome for the active Core session");
		WriteJsonPipeFrame(reconnectClient, FrameKind::Request, 11, {
			{"operation", "objects.handle.issue"},
			{"session_id", "fixture-pipe-session"},
			{"timeout_ms", 5000},
			{"data", {{"index", 7}}}
		});
		WaitUntil(
			[&executor] { return executor.HasPending(); },
			"Reconnected Host request did not enter the game-thread queue");
		CloseHandle(reconnectClient);
		WaitUntil(
			[&server] { return server.Diagnostics().PendingRequests == 0; },
			"Mid-request Host disconnect did not cancel and retire pending work");
		Require(!executor.HasPending(), "Disconnected Host retained queued game-thread work");

		Require(server.Stop(std::chrono::milliseconds(5000)), "Named-pipe workers did not join on stop");
		const NamedPipeServerDiagnostics pipeDiagnostics = server.Diagnostics();
		Require(
			!pipeDiagnostics.Listening
				&& pipeDiagnostics.WorkerCount == 0
				&& !pipeDiagnostics.EventWriterRunning
				&& pipeDiagnostics.AcceptedConnections == 3
				&& pipeDiagnostics.RejectedConnections >= expectedRejected
				&& pipeDiagnostics.CompletedRequests >= 3
				&& pipeDiagnostics.CancelledRequests >= 2
				&& pipeDiagnostics.EnqueuedEvents == 1
				&& pipeDiagnostics.SentEvents == 1
				&& pipeDiagnostics.DroppedEvents == 0,
			"Named-pipe diagnostics did not account for lifecycle, request, and cancellation state");
		Require(executor.DisableAndDrain(), "Pipe game-thread executor did not drain");
		Require(engine.Stop(), "Pipe EngineFacade did not stop");
		Require(
			runtime.BeginStopping()
				&& runtime.WaitForRequests(std::chrono::milliseconds(100))
				&& runtime.MarkStopped(),
			"Pipe runtime did not reach Stopped after transport drain");
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

	void TestBytePatternScanner()
	{
		using UExplorer::Platform::FindBytePatternOffset;
		using UExplorer::Platform::TryParseBytePattern;

		std::vector<int> pattern;
		Require(TryParseBytePattern("AA BB ? DD", pattern),
			"Valid byte pattern was rejected");
		Require(pattern == std::vector<int>({0xAA, 0xBB, -1, 0xDD}),
			"Byte pattern parser changed wildcard semantics");
		const std::array<std::uint8_t, 12> bytes{
			0xAA, 0xBB, 0x01, 0xDD,
			0xAA, 0xBB, 0x02, 0xDD,
			0xAA, 0xBB, 0x03, 0xDD
		};
		Require(FindBytePatternOffset(bytes, pattern, 0) == 0,
			"Pattern scanner missed the first match");
		Require(FindBytePatternOffset(bytes, pattern, 1) == 4,
			"Pattern scanner did not retain one skipped match");
		Require(FindBytePatternOffset(bytes, pattern, 2) == 8,
			"Pattern scanner missed a match at the final candidate offset");
		Require(!FindBytePatternOffset(bytes, pattern, 3),
			"Pattern scanner returned a match past the skip count");
		const std::array<int, 1> invalidPattern{-2};
		Require(!FindBytePatternOffset(bytes, invalidPattern),
			"Pattern scanner accepted an invalid byte token");

		for (const std::string_view malformed : {
			std::string_view{},
			std::string_view{"A"},
			std::string_view{"GG"},
			std::string_view{"AA BBX"},
			std::string_view{"?A"}
		})
		{
			Require(!TryParseBytePattern(malformed, pattern),
				"Malformed byte pattern was accepted");
			Require(pattern.empty(),
				"Failed byte-pattern parsing exposed a partial result");
		}
	}

	void TestPeImageInspectionAndEngineVersionProbe()
	{
		using namespace UExplorer::Platform;
		using namespace UExplorer::Runtime;

		constexpr std::size_t imageSize = 0x12000;
		constexpr std::size_t ntOffset = 0x80;
		constexpr std::size_t sectionVirtualAddress = 0x200;
		std::vector<std::byte> imageBytes(imageSize);

		IMAGE_DOS_HEADER dos{};
		dos.e_magic = IMAGE_DOS_SIGNATURE;
		dos.e_lfanew = static_cast<LONG>(ntOffset);
		StoreFixtureValue(imageBytes, 0, dos);

		IMAGE_NT_HEADERS64 nt{};
		nt.Signature = IMAGE_NT_SIGNATURE;
		nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
		nt.FileHeader.NumberOfSections = 1;
		nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
		nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
		nt.OptionalHeader.SizeOfImage = static_cast<DWORD>(imageSize);
		nt.OptionalHeader.SizeOfHeaders = static_cast<DWORD>(sectionVirtualAddress);
		StoreFixtureValue(imageBytes, ntOffset, nt);

		IMAGE_SECTION_HEADER section{};
		constexpr std::array<std::uint8_t, 6> sectionName{
			'.', 'r', 'd', 'a', 't', 'a'
		};
		std::copy(sectionName.begin(), sectionName.end(), section.Name);
		section.VirtualAddress = static_cast<DWORD>(sectionVirtualAddress);
		section.Misc.VirtualSize = static_cast<DWORD>(imageSize - sectionVirtualAddress);
		section.Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
		const std::size_t sectionHeaderOffset = ntOffset + sizeof(IMAGE_NT_HEADERS64);
		StoreFixtureValue(imageBytes, sectionHeaderOffset, section);

		constexpr std::string_view marker = "++UE5+Release-5.4.2";
		const std::size_t markerOffset = sectionVirtualAddress + 64 * 1024 - 8;
		Require(markerOffset + marker.size() < imageBytes.size(),
			"Engine version fixture marker is out of bounds");
		std::memcpy(imageBytes.data() + markerOffset, marker.data(), marker.size());

		PeImageView image;
		const PeImageResult inspected = InspectPeImage(
			reinterpret_cast<std::uintptr_t>(imageBytes.data()),
			image);
		Require(inspected.Ok(), "Valid synthetic PE image was rejected");
		const PeSectionView* rdata = image.FindSection(".rdata");
		Require(rdata != nullptr
			&& rdata->Address == reinterpret_cast<std::uintptr_t>(imageBytes.data())
				+ sectionVirtualAddress
			&& rdata->IsReadable(),
			"Synthetic PE section metadata was not preserved");
		const EngineVersionProbeResult version = ProbeEngineVersion(image);
		Require(version.Ok() && version.Version == "5.4.2",
			"Bounded readable-section engine version probe failed");

		const auto MarkerBytes = [](const std::string_view text) {
			return std::span<const std::byte>(
				reinterpret_cast<const std::byte*>(text.data()),
				text.size());
		};
		Require(ParseEngineVersionMarkers(MarkerBytes("UE4+Release-4.27.2")).Ok(),
			"Valid UE4 marker was rejected");
		Require(ParseEngineVersionMarkers(MarkerBytes("++UE5+Release-5..4")).Error
			== EngineVersionProbeError::MarkerNotFound,
			"Malformed engine version marker was accepted");
		Require(ParseEngineVersionMarkers(MarkerBytes("++UE5+Release-6.0")).Error
			== EngineVersionProbeError::MarkerNotFound,
			"Unsupported engine major version marker was accepted");

		std::vector<std::byte> invalidDos = imageBytes;
		IMAGE_DOS_HEADER brokenDos = dos;
		brokenDos.e_magic = 0;
		StoreFixtureValue(invalidDos, 0, brokenDos);
		Require(InspectPeImage(
			reinterpret_cast<std::uintptr_t>(invalidDos.data()), image).Error
			== PeImageError::InvalidDosHeader,
			"Invalid DOS header was accepted");

		std::vector<std::byte> invalidSection = imageBytes;
		IMAGE_SECTION_HEADER brokenSection = section;
		brokenSection.Misc.VirtualSize = static_cast<DWORD>(imageSize);
		StoreFixtureValue(invalidSection, sectionHeaderOffset, brokenSection);
		Require(InspectPeImage(
			reinterpret_cast<std::uintptr_t>(invalidSection.data()), image).Error
			== PeImageError::InvalidSection,
			"Out-of-image PE section was accepted");

		PeImageView loadedImage;
		Require(InspectLoadedPeImage(nullptr, loadedImage).Ok(),
			"Current x64 executable PE image could not be inspected");
		Require(InspectLoadedPeImage(
			L"UExplorer-fixture-module-that-does-not-exist.dll", loadedImage).Error
			== PeImageError::ModuleNotLoaded,
			"Missing module lookup did not return an explicit error");

		SYSTEM_INFO systemInfo{};
		GetSystemInfo(&systemInfo);
		void* noAccess = VirtualAlloc(
			nullptr,
			systemInfo.dwPageSize,
			MEM_RESERVE | MEM_COMMIT,
			PAGE_NOACCESS);
		Require(noAccess != nullptr, "Engine version no-access fixture allocation failed");
		struct NoAccessGuard
		{
			void* Address;
			~NoAccessGuard() { if (Address) VirtualFree(Address, 0, MEM_RELEASE); }
		} noAccessGuard{noAccess};
		PeImageView inaccessible{
			.Base = reinterpret_cast<std::uintptr_t>(noAccess),
			.Size = systemInfo.dwPageSize,
			.Sections = {{
				.Name = {},
				.HeaderAddress = 0,
				.Address = reinterpret_cast<std::uintptr_t>(noAccess),
				.Size = systemInfo.dwPageSize,
				.Characteristics = IMAGE_SCN_MEM_READ
			}}
		};
		const EngineVersionProbeResult inaccessibleResult = ProbeEngineVersion(inaccessible);
		Require(inaccessibleResult.Error == EngineVersionProbeError::MemoryReadFailed
			&& inaccessibleResult.MemoryFailure == MemoryError::AccessDenied,
			"Unreadable PE section did not return a typed probe error");
	}

	void TestGlobalPointerDiscovery()
	{
		using namespace OffsetFinder;
		using namespace UExplorer::Platform;

		constexpr std::uintptr_t base = 0x140000000;
		constexpr std::uintptr_t expected = 0x000001F000100000;
		PeImageView image{
			.Base = base,
			.Size = 0x10000,
			.Sections = {
				{
					.Name = {'.', 't', 'e', 'x', 't'},
					.Address = base + 0x1000,
					.Size = 0x1000,
					.Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE
				},
				{
					.Name = {'.', 'd', 'a', 't', 'a'},
					.Address = base + 0x3000,
					.Size = 0x1000,
					.Characteristics = IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE
				}
			}
		};

		const GlobalPointerCandidateObservation valid{
			.SlotAddress = base + 0x3010,
			.ExpectedTarget = expected,
			.FirstValue = expected,
			.SecondValue = expected,
			.ExpectedTargetFromObjectArray = true,
			.ExpectedTargetTypeValidated = true,
			.FirstReadSucceeded = true,
			.SecondReadSucceeded = true
		};
		const GlobalPointerDiscoveryReport selected =
			ResolveGlobalPointerCandidates(image, std::span(&valid, 1));
		Require(selected.Ok()
			&& selected.SelectedOffset == 0x3010
			&& selected.Confidence == "high"
			&& selected.Candidates.size() == 1
			&& selected.Candidates.front().Accepted,
			"Unique stable typed global-pointer candidate was rejected");

		const std::array duplicate{valid, valid};
		Require(ResolveGlobalPointerCandidates(image, duplicate).Ok(),
			"Duplicate observations of one slot were treated as ambiguity");

		GlobalPointerCandidateObservation second = valid;
		second.SlotAddress = base + 0x3020;
		const std::array ambiguous{valid, second};
		Require(ResolveGlobalPointerCandidates(image, ambiguous).Error
			== GlobalPointerDiscoveryError::AmbiguousCandidates,
			"Multiple validated global-pointer slots did not fail closed");

		GlobalPointerCandidateObservation executable = valid;
		executable.SlotAddress = base + 0x1010;
		const GlobalPointerDiscoveryReport executableRejected =
			ResolveGlobalPointerCandidates(image, std::span(&executable, 1));
		Require(executableRejected.Error == GlobalPointerDiscoveryError::NoValidatedCandidate
			&& executableRejected.Candidates.front().RejectionCode
				== "GLOBAL_POINTER_SLOT_NOT_WRITABLE_DATA",
			"Executable-section pointer candidate was accepted as mutable engine state");

		GlobalPointerCandidateObservation unaligned = valid;
		unaligned.SlotAddress = base + 0x3011;
		const GlobalPointerDiscoveryReport unalignedRejected =
			ResolveGlobalPointerCandidates(image, std::span(&unaligned, 1));
		Require(unalignedRejected.Error == GlobalPointerDiscoveryError::NoValidatedCandidate
			&& unalignedRejected.Candidates.front().RejectionCode
				== "GLOBAL_POINTER_SLOT_UNALIGNED",
			"Unaligned global-pointer slot was accepted");

		GlobalPointerCandidateObservation changed = valid;
		changed.SecondValue = expected + 8;
		const GlobalPointerDiscoveryReport unstable =
			ResolveGlobalPointerCandidates(image, std::span(&changed, 1));
		Require(unstable.Error == GlobalPointerDiscoveryError::NoValidatedCandidate
			&& unstable.Candidates.front().RejectionCode
				== "GLOBAL_POINTER_TARGET_UNSTABLE",
			"Changing global-pointer candidate did not fail closed");
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
			static_cast<std::size_t>(systemInfo.dwPageSize) * 2,
			MEM_RESERVE | MEM_COMMIT,
			PAGE_READWRITE);
		Require(allocation != nullptr, "SafeMemory fixture allocation failed");
		struct AllocationGuard
		{
			void* Address;
			~AllocationGuard() { if (Address) VirtualFree(Address, 0, MEM_RELEASE); }
		} allocationGuard{allocation};

		const auto address = reinterpret_cast<std::uintptr_t>(allocation);
		Require(ValidateReadableMemory(
			address,
			static_cast<std::size_t>(systemInfo.dwPageSize) * 2).Ok(),
			"SafeMemory rejected a committed readable range");
		Require(ValidateReadableMemory(0, 1).Error == MemoryError::InvalidRange,
			"SafeMemory readable-range validation accepted nullptr");
		DWORD boundaryProtection = 0;
		Require(VirtualProtect(
			reinterpret_cast<void*>(address + systemInfo.dwPageSize),
			systemInfo.dwPageSize,
			PAGE_NOACCESS,
			&boundaryProtection) != FALSE,
			"SafeMemory boundary fixture setup failed");
		Require(ValidateReadableMemory(
			address + systemInfo.dwPageSize - 1,
			2).Error == MemoryError::AccessDenied,
			"SafeMemory missed a no-access page inside the requested range");
		DWORD restoredBoundaryProtection = 0;
		Require(VirtualProtect(
			reinterpret_cast<void*>(address + systemInfo.dwPageSize),
			systemInfo.dwPageSize,
			boundaryProtection,
			&restoredBoundaryProtection) != FALSE,
			"SafeMemory boundary fixture cleanup failed");
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

	void TestPostRenderFrameClientOwnershipAndDrain()
	{
		using namespace UExplorer::Runtime;

		{
			GameThreadExecutor executor;
			PostRenderPumpBackend backend(executor);
			FrameClientProbe first;
			FrameClientProbe second;
			Require(executor.Enable(&FakeProcessEvent), "Frame-client executor did not enable");
			Require(backend.AttachFrameClient(first), "PostRender frame client did not attach");
			Require(!backend.AttachFrameClient(second), "PostRender accepted two frame clients");
			Require(backend.HasFrameClient(), "Attached PostRender frame client was not published");
			backend.Tick();
			Require(
				first.Calls.load() == 1
					&& first.LastBudget.load() == PostRenderPumpBackend::kFrameWorkBudget,
				"PostRender did not pump the attached frame client with the fixed budget");
			Require(
				backend.DetachFrameClient(first, std::chrono::seconds(1)),
				"PostRender frame client did not detach");
			const int detachedCalls = first.Calls.load();
			backend.Tick();
			Require(
				first.Calls.load() == detachedCalls,
				"Post-stop callback was allowed to run owned frame-client work");
			Require(backend.AttachFrameClient(second), "PostRender frame-client barrier did not reset");
			backend.Tick();
			Require(second.Calls.load() == 1, "Replacement frame client was not pumped");
			Require(
				backend.DetachFrameClient(second, std::chrono::seconds(1)),
				"Replacement frame client did not drain");
			Require(executor.DisableAndDrain(), "Frame-client executor did not drain");
		}

		{
			GameThreadExecutor executor;
			PostRenderPumpBackend backend(executor);
			FrameClientProbe blocking;
			Require(executor.Enable(&FakeProcessEvent), "Blocking frame-client executor did not enable");
			Require(backend.AttachFrameClient(blocking), "Blocking frame client did not attach");
			blocking.Block.store(true, std::memory_order_release);
			auto inFlightTick = std::async(std::launch::async, [&backend] { backend.Tick(); });
			WaitUntil(
				[&blocking] { return blocking.Entered.load(std::memory_order_acquire); },
				"Blocking PostRender frame client was not entered");
			Require(
				!backend.DetachFrameClient(blocking, std::chrono::milliseconds(20)),
				"PostRender frame-client detach ignored an in-flight callback");
			Require(!backend.HasFrameClient(), "Draining frame client remained reachable to new ticks");
			Require(backend.FrameClientInFlight() == 1, "Frame-client in-flight diagnostic was incorrect");
			blocking.Block.store(false, std::memory_order_release);
			inFlightTick.get();
			Require(
				backend.DetachFrameClient(blocking, std::chrono::seconds(1)),
				"PostRender frame-client detach could not be retried after drain");
			Require(backend.FrameClientInFlight() == 0, "Drained frame client retained a callback lease");
			Require(executor.DisableAndDrain(), "Blocking frame-client executor did not drain");
		}

		{
			GameThreadExecutor executor;
			PostRenderPumpBackend backend(executor);
			FrameClientProbe wrongThreadProbe;
			Require(executor.Enable(&FakeProcessEvent), "Wrong-thread frame executor did not enable");
			Require(backend.AttachFrameClient(wrongThreadProbe), "Wrong-thread frame probe did not attach");
			backend.Tick();
			const int stableCalls = wrongThreadProbe.Calls.load(std::memory_order_acquire);
			auto wrongThreadTick = std::async(std::launch::async, [&backend] { backend.Tick(); });
			wrongThreadTick.get();
			Require(
				wrongThreadProbe.Calls.load(std::memory_order_acquire) == stableCalls
					&& !executor.GetDiagnostics().PumpThreadStable,
				"PostRender dispatched frame work after detecting a pump-thread mismatch");
			Require(
				backend.DetachFrameClient(wrongThreadProbe, std::chrono::seconds(1)),
				"Wrong-thread frame probe did not detach");
			Require(executor.DisableAndDrain(), "Wrong-thread frame executor did not drain");
		}
	}

	void TestGameThreadFrameSchedulerBudgetFairnessAndDrain()
	{
		using namespace UExplorer::Runtime;

		GameThreadFrameScheduler scheduler;
		std::array<FrameClientProbe, GameThreadFrameScheduler::kMaxClients + 1> clients;
		for (std::size_t index = 0; index < GameThreadFrameScheduler::kMaxClients; ++index)
		{
			clients[index].ConsumeBudget.store(true, std::memory_order_release);
			clients[index].MoreWorkPending.store(true, std::memory_order_release);
			Require(scheduler.AttachClient(clients[index]), "Frame scheduler client did not attach");
		}
		Require(
			!scheduler.AttachClient(clients[0])
				&& !scheduler.AttachClient(clients.back()),
			"Frame scheduler accepted a duplicate or exceeded its client capacity");

		const IGameThreadFrameClient::PumpResult fullFrame = scheduler.PumpFrame(
			GameThreadFrameScheduler::kMaxFrameWorkBudget);
		Require(
			fullFrame.WorkConsumed == GameThreadFrameScheduler::kMaxFrameWorkBudget,
			"Frame scheduler exceeded or under-consumed its total work budget");
		for (std::size_t index = 0; index < GameThreadFrameScheduler::kMaxClients; ++index)
		{
			Require(
				clients[index].Calls.load(std::memory_order_acquire) == 1
					&& clients[index].LastBudget.load(std::memory_order_acquire)
						== GameThreadFrameScheduler::kClientQuantum,
				"Frame scheduler did not distribute the first round fairly");
		}
		const GameThreadFrameSchedulerDiagnostics fullDiagnostics = scheduler.Diagnostics();
		Require(
			fullDiagnostics.AttachedClients == GameThreadFrameScheduler::kMaxClients
				&& fullDiagnostics.LastFrameWorkConsumed
					== GameThreadFrameScheduler::kMaxFrameWorkBudget
				&& fullDiagnostics.LastFrameDispatches == GameThreadFrameScheduler::kMaxClients,
			"Frame scheduler diagnostics did not report bounded aggregate work");
		for (std::size_t index = 0; index < GameThreadFrameScheduler::kMaxClients; ++index)
		{
			Require(
				scheduler.DetachClient(clients[index], std::chrono::seconds(1)),
				"Frame scheduler client did not detach");
		}

		GameThreadFrameScheduler fairScheduler;
		FrameClientProbe fairFirst;
		FrameClientProbe fairSecond;
		fairFirst.ConsumeBudget.store(true, std::memory_order_release);
		fairFirst.MoreWorkPending.store(true, std::memory_order_release);
		fairSecond.ConsumeBudget.store(true, std::memory_order_release);
		fairSecond.MoreWorkPending.store(true, std::memory_order_release);
		Require(
			fairScheduler.AttachClient(fairFirst) && fairScheduler.AttachClient(fairSecond),
			"Fairness probes did not attach");
		Require(
			fairScheduler.PumpFrame(1).WorkConsumed == 1
				&& fairScheduler.PumpFrame(1).WorkConsumed == 1
				&& fairFirst.Calls.load(std::memory_order_acquire) == 1
				&& fairSecond.Calls.load(std::memory_order_acquire) == 1,
			"Frame scheduler did not rotate a constrained budget between clients");
		Require(fairScheduler.DetachClient(fairFirst), "First fairness probe did not detach");
		Require(fairScheduler.DetachClient(fairSecond), "Second fairness probe did not detach");

		GameThreadFrameScheduler guardedScheduler;
		FrameClientProbe blocking;
		blocking.Block.store(true, std::memory_order_release);
		blocking.ConsumeBudget.store(true, std::memory_order_release);
		blocking.MoreWorkPending.store(true, std::memory_order_release);
		Require(guardedScheduler.AttachClient(blocking), "Blocking scheduler client did not attach");
		auto pumping = std::async(std::launch::async, [&guardedScheduler] {
			return guardedScheduler.PumpFrame(GameThreadFrameScheduler::kClientQuantum);
		});
		WaitUntil(
			[&blocking] { return blocking.Entered.load(std::memory_order_acquire); },
			"Blocking scheduler client was not entered");
		Require(
			guardedScheduler.PumpFrame(1).WorkConsumed == 0
				&& guardedScheduler.Diagnostics().ConcurrentPumpRejectedCount == 1,
			"Frame scheduler allowed concurrent mutation of its dispatch cursor");
		Require(
			!guardedScheduler.DetachClient(blocking, std::chrono::milliseconds(20))
				&& !guardedScheduler.HasClient(blocking)
				&& guardedScheduler.Diagnostics().DrainingClients == 1,
			"Frame scheduler detach did not withdraw and retain an in-flight owner");
		blocking.Block.store(false, std::memory_order_release);
		pumping.get();
		Require(
			guardedScheduler.DetachClient(blocking, std::chrono::seconds(1))
				&& guardedScheduler.Diagnostics().InFlightCallbacks == 0,
			"Frame scheduler client detach could not be retried after drain");
		const int detachedCalls = blocking.Calls.load(std::memory_order_acquire);
		(void)guardedScheduler.PumpFrame(GameThreadFrameScheduler::kClientQuantum);
		Require(
			blocking.Calls.load(std::memory_order_acquire) == detachedCalls,
			"Detached scheduler client remained reachable");

		GameThreadFrameScheduler defensiveScheduler;
		FrameClientProbe zeroProgress;
		zeroProgress.MoreWorkPending.store(true, std::memory_order_release);
		Require(defensiveScheduler.AttachClient(zeroProgress), "Zero-progress probe did not attach");
		Require(
			defensiveScheduler.PumpFrame(GameThreadFrameScheduler::kMaxFrameWorkBudget)
				.WorkConsumed == 0
				&& zeroProgress.Calls.load(std::memory_order_acquire) == 1
				&& defensiveScheduler.Diagnostics().ZeroProgressCount == 1,
			"Frame scheduler spun on a client that reported no progress");
		Require(defensiveScheduler.DetachClient(zeroProgress), "Zero-progress probe did not detach");

		FrameClientProbe overReporter;
		overReporter.OverReport.store(true, std::memory_order_release);
		Require(defensiveScheduler.AttachClient(overReporter), "Contract-violation probe did not attach");
		Require(
			defensiveScheduler.PumpFrame(GameThreadFrameScheduler::kClientQuantum)
				.WorkConsumed == GameThreadFrameScheduler::kClientQuantum
				&& defensiveScheduler.Diagnostics().ContractViolationCount == 1,
			"Frame scheduler did not contain a client budget-contract violation");
		Require(defensiveScheduler.DetachClient(overReporter), "Contract-violation probe did not detach");
		Require(
			defensiveScheduler.PumpFrame(0).WorkConsumed == 0
				&& defensiveScheduler.PumpFrame(
					GameThreadFrameScheduler::kMaxFrameWorkBudget + 1).WorkConsumed == 0
				&& defensiveScheduler.Diagnostics().InvalidBudgetCount == 2,
			"Frame scheduler accepted an invalid aggregate budget");

		GameThreadFrameScheduler timedScheduler;
		FrameClientProbe slow;
		FrameClientProbe afterSlow;
		slow.ConsumeBudget.store(true, std::memory_order_release);
		slow.MoreWorkPending.store(true, std::memory_order_release);
		slow.SleepMilliseconds.store(3, std::memory_order_release);
		afterSlow.ConsumeBudget.store(true, std::memory_order_release);
		afterSlow.MoreWorkPending.store(true, std::memory_order_release);
		Require(
			timedScheduler.AttachClient(slow) && timedScheduler.AttachClient(afterSlow),
			"Time-budget probes did not attach");
		(void)timedScheduler.PumpFrame(GameThreadFrameScheduler::kMaxFrameWorkBudget);
		Require(
			slow.Calls.load(std::memory_order_acquire) == 1
				&& afterSlow.Calls.load(std::memory_order_acquire) == 0
				&& timedScheduler.Diagnostics().TimeBudgetExhaustions == 1,
			"Frame scheduler continued dispatch after exhausting its frame-time budget");
		Require(timedScheduler.DetachClient(slow), "Slow probe did not detach");
		Require(timedScheduler.DetachClient(afterSlow), "Post-slow probe did not detach");
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

	int RunHostSessionFixture()
	{
		using namespace UExplorer::IPC;
		using namespace UExplorer::Runtime;
		using namespace UExplorer::Services;

		constexpr char sessionId[] = "fixture-host-session";
		constexpr std::uint64_t contextGeneration = 901;
		const std::uint32_t processId = GetCurrentProcessId();

		CoreRuntime runtime;
		Require(runtime.BeginInitialize(sessionId), "Host fixture runtime did not initialize");
		const auto context = MakeEngineContext(
			contextGeneration,
			true,
			true,
			nullptr,
			processId);
		Require(runtime.PublishContext(context), "Host fixture runtime rejected its EngineContext");

		GameThreadExecutor executor;
		Require(executor.Enable(&FakeProcessEvent), "Host fixture game-thread executor did not enable");
		FakeHandleIdentitySource source;
		source.Generation = contextGeneration;
		EngineFacade engine(context, sessionId, source);

		EngineSnapshot snapshot{
			.SessionId = sessionId,
			.ContextGeneration = contextGeneration,
			.Generation = 1,
			.CapturedAtMonotonicUs = 2'000'000,
			.CaptureDurationUs = 250,
			.SourceObjectCount = 8,
			.SkippedSlots = 5
		};
		snapshot.Objects.push_back(MakeSnapshotObject(
			sessionId,
			contextGeneration,
			1,
			EngineObjectKind::Class));
		snapshot.Objects.push_back(MakeSnapshotObject(
			sessionId,
			contextGeneration,
			4,
			EngineObjectKind::Function));
		snapshot.Objects.push_back(MakeSnapshotObject(
			sessionId,
			contextGeneration,
			7,
			EngineObjectKind::Object));
		Require(
			engine.Snapshots().Publish(std::move(snapshot)).Ok(),
			"Host fixture immutable snapshot did not publish");

		FakeCoreStatusDiagnostics statusDiagnostics;
		CoreCommandService service(runtime, executor, engine, statusDiagnostics);
		Require(service.IsConfigured(), "Host fixture command service was not configured");

		std::atomic<bool> shutdownObserved{false};
		NamedPipeRpcServer server(
			runtime,
			service,
			executor,
			[&shutdownObserved] { shutdownObserved.store(true, std::memory_order_release); });
		Require(server.Start(), "Host fixture Named Pipe server did not bind");

		RuntimeProbes probes;
		probes.GameThreadExecutorEnabled = true;
		probes.GameThreadPumpObserved = true;
		probes.GameThreadPumpThreadStable = true;
		probes.GameThreadPumpActive = true;
		probes.SafeMemoryEnabled = true;
		probes.ObjectIdentitySourceEnabled = true;
		probes.ObjectHandleValidationEnabled = true;
		probes.FunctionHandleValidationEnabled = true;
		probes.ObjectSnapshotPublished = true;
		probes.NamedPipeListening = server.IsListening();
		Require(
			runtime.PublishCapabilities(BuildCoreCapabilities(*context, probes))
				&& runtime.TryMarkReady(RequiredReadyCapabilities())
				&& server.OpenAdmissions(),
			"Host fixture did not reach factual Core Ready");

		const nlohmann::json ready = {
			{"pid", processId},
			{"session_id", sessionId},
			{"snapshot_generation", 1},
			{"snapshot_records", 3}
		};
		std::cout << "UEXPLORER_HOST_FIXTURE_READY " << ready.dump() << '\n' << std::flush;

		bool eventPublished = false;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (!shutdownObserved.load(std::memory_order_acquire))
		{
			if (!eventPublished && server.Diagnostics().AcceptedConnections > 0)
			{
				const auto timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now().time_since_epoch()).count();
				Require(timestamp >= 0, "Host fixture monotonic clock was negative");
				Require(
					server.PublishEvent(
						"fixture.ready",
						static_cast<std::uint64_t>(timestamp),
						{
							{"source", "cpp-core"},
							{"snapshot_generation", 1}
						}) == EventPublishResult::Accepted,
					"Host fixture event was not admitted exactly once");
				eventPublished = true;
			}
			if (std::chrono::steady_clock::now() >= deadline)
				throw std::runtime_error("Host fixture timed out waiting for an exact Shutdown");
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		Require(eventPublished, "Host fixture shut down before publishing its real Core event");
		Require(server.Stop(std::chrono::milliseconds(5000)), "Host fixture Pipe workers did not join");
		const NamedPipeServerDiagnostics pipe = server.Diagnostics();
		Require(
			pipe.EnqueuedEvents == 1
				&& pipe.SentEvents == 1
				&& pipe.DroppedEvents == 0,
			"Host fixture event transport diagnostics are not exact");
		Require(executor.DisableAndDrain(), "Host fixture game-thread executor did not drain");
		Require(engine.Stop(), "Host fixture EngineFacade did not stop");
		Require(
			runtime.BeginStopping()
				&& runtime.WaitForRequests(std::chrono::milliseconds(100))
				&& runtime.MarkStopped(),
			"Host fixture runtime did not reach Stopped");
		return 0;
	}
}


void TestWorldSnapshotQueries()
{
	using namespace UExplorer::Runtime;
	using UExplorer::Services::WorldCommandService;

	const auto handle = [](const std::int32_t index, const std::uintptr_t address) {
		return ObjectHandle{
			.SessionId = "world-fixture",
			.ContextGeneration = 81,
			.Index = index,
			.SerialNumber = index + 100,
			.Address = address,
			.ClassFingerprint = 0xA000u + static_cast<std::uint64_t>(index)
		};
	};
	WorldSnapshot snapshot{
		.SessionId = "world-fixture",
		.ContextGeneration = 81,
		.Generation = 3,
		.ObjectSnapshotGeneration = 7,
		.TypeSnapshotGeneration = 9,
		.CapturedAtMonotonicUs = 1000,
		.CaptureDurationUs = 25,
		.World = {
			.Handle = handle(1, 0x1000),
			.Name = "FixtureWorld",
			.FullPath = "/Game/Maps/Fixture.Fixture",
			.ClassPath = "/Script/Engine.World"
		},
		.GameMode = {.State = WorldReferenceState::NotPresent},
		.GameState = {.State = WorldReferenceState::NotPresent},
		.PlayerController = {
			.State = WorldReferenceState::Unavailable,
			.ReasonCode = "WORLD_LOCAL_PLAYER_CHAIN_UNAVAILABLE",
			.Reason = "Fixture has no validated LocalPlayers traversal"
		},
		.Pawn = {
			.State = WorldReferenceState::Unavailable,
			.ReasonCode = "WORLD_LOCAL_PLAYER_CHAIN_UNAVAILABLE",
			.Reason = "Fixture has no validated local PlayerController"
		},
		.ComponentsAvailable = true,
		.Levels = {{
			.Object = {
				.Handle = handle(2, 0x2000),
				.Name = "PersistentLevel",
				.FullPath = "/Game/Maps/Fixture.Fixture.PersistentLevel",
				.ClassPath = "/Script/Engine.Level"
			},
			.ActorCount = 3
		}},
		.Actors = {
			{
				.Object = {
					.Handle = handle(3, 0x3000),
					.Name = "FixtureActor",
					.FullPath = "/Game/Maps/Fixture.Fixture.PersistentLevel.FixtureActor",
					.ClassPath = "/Script/Engine.Actor"
				},
				.Level = handle(2, 0x2000),
				.RootComponent = {
					.State = WorldReferenceState::Present,
					.Object = WorldSnapshotObject{
						.Handle = handle(6, 0x6000),
						.Name = "Root",
						.FullPath = "/Game/Maps/Fixture.Fixture.PersistentLevel.FixtureActor.Root",
						.ClassPath = "/Script/Engine.SceneComponent"
					}
				},
				.ComponentCount = 2
			},
			{
				.Object = {
					.Handle = handle(4, 0x4000),
					.Name = "UnmatchedPawn",
					.FullPath = "/Game/Maps/Fixture.Fixture.PersistentLevel.UnmatchedPawn",
					.ClassPath = "/Script/Engine.Pawn"
				},
				.Level = handle(2, 0x2000),
				.RootComponent = {.State = WorldReferenceState::NotPresent}
			},
			{
				.Object = {
					.Handle = handle(5, 0x5000),
					.Name = "FixtureActorTwo",
					.FullPath = "/Game/Maps/Fixture.Fixture.PersistentLevel.FixtureActorTwo",
					.ClassPath = "/Script/Engine.Actor"
				},
				.Level = handle(2, 0x2000),
				.RootComponent = {
					.State = WorldReferenceState::Present,
					.Object = WorldSnapshotObject{
						.Handle = handle(8, 0x8000),
						.Name = "RootTwo",
						.FullPath = "/Game/Maps/Fixture.Fixture.PersistentLevel.FixtureActorTwo.RootTwo",
						.ClassPath = "/Script/Engine.SceneComponent"
					}
				},
				.ComponentCount = 1
			}
		},
		.Components = {
			{
				.Object = {
					.Handle = handle(6, 0x6000),
					.Name = "Root",
					.FullPath = "/Game/Maps/Fixture.Fixture.PersistentLevel.FixtureActor.Root",
					.ClassPath = "/Script/Engine.SceneComponent"
				},
				.Owner = handle(3, 0x3000)
			},
			{
				.Object = {
					.Handle = handle(7, 0x7000),
					.Name = "Mesh",
					.FullPath = "/Game/Maps/Fixture.Fixture.PersistentLevel.FixtureActor.Mesh",
					.ClassPath = "/Script/Engine.StaticMeshComponent"
				},
				.Owner = handle(3, 0x3000)
			},
			{
				.Object = {
					.Handle = handle(8, 0x8000),
					.Name = "RootTwo",
					.FullPath = "/Game/Maps/Fixture.Fixture.PersistentLevel.FixtureActorTwo.RootTwo",
					.ClassPath = "/Script/Engine.SceneComponent"
				},
				.Owner = handle(5, 0x5000)
			}
		}
	};
	WorldSnapshotStore store("world-fixture", 81);
	const WorldSnapshotPublishResult published = store.Publish(std::move(snapshot));
	Require(published.Ok(), "A coherent immutable world snapshot was rejected");

	const auto inspect = WorldCommandService::Execute(
		"world.inspect",
		nlohmann::json::object(),
		store.Current());
	Require(
		inspect.Ok()
			&& inspect.Data.at("level_count") == 1
			&& inspect.Data.at("actor_count") == 3
			&& inspect.Data.at("components").at("state") == "available"
			&& inspect.Data.at("components").at("count") == 3,
		"World inspection did not expose exact immutable counts");
	const auto shortcuts = WorldCommandService::Execute(
		"world.shortcuts",
		nlohmann::json::object(),
		store.Current());
	Require(
		shortcuts.Ok()
			&& shortcuts.Data.at("game_mode").at("state") == "not_present"
			&& shortcuts.Data.at("player_controller").at("state") == "unavailable",
		"World shortcuts fabricated a global object instead of preserving relation state");
	const auto actorDetail = WorldCommandService::Execute(
		"world.actor.get",
		{{"actor", {
			{"session_id", "world-fixture"},
			{"context_generation", 81},
			{"index", 3},
			{"serial", 103},
			{"address", "0x3000"},
			{"class_fingerprint", "000000000000A003"}
		}}, {"world_snapshot_generation", 3}},
		store.Current());
	Require(
		actorDetail.Ok()
			&& actorDetail.Data.at("actor").at("handle").at("index") == 3
			&& actorDetail.Data.at("root_component").at("state") == "present"
			&& actorDetail.Data.at("root_component").at("object").at("handle").at("index") == 6
			&& actorDetail.Data.at("components").at("count") == 2
			&& actorDetail.Data.at("transform").at("state") == "unavailable",
		"World actor detail lost its exact handle, root component, or explicit transform state");
	const nlohmann::json actorHandle = actorDetail.Data.at("actor").at("handle");
	const auto components = WorldCommandService::Execute(
		"world.actor.components",
		{{"actor", actorHandle},
		 {"world_snapshot_generation", 3},
		 {"cursor", nullptr},
		 {"limit", 1}},
		store.Current());
	Require(
		components.Ok()
			&& components.Data.at("components").size() == 1
			&& components.Data.at("components").at(0).at("handle").at("index") == 6
			&& components.Data.at("count") == 2
			&& components.Data.at("has_more") == true,
		"World component paging did not bind the first page to the exact Actor");
	const auto componentsNext = WorldCommandService::Execute(
		"world.actor.components",
		{{"actor", actorHandle},
		 {"world_snapshot_generation", 3},
		 {"cursor", components.Data.at("next_cursor")},
		 {"limit", 1}},
		store.Current());
	Require(
		componentsNext.Ok()
			&& componentsNext.Data.at("components").size() == 1
			&& componentsNext.Data.at("components").at(0).at("handle").at("index") == 7
			&& componentsNext.Data.at("has_more") == false,
		"World component continuation crossed Actor ownership or lost its terminal state");
	nlohmann::json staleActorHandle = actorHandle;
	staleActorHandle["serial"] = 999;
	const auto staleActor = WorldCommandService::Execute(
		"world.actor.get",
		{{"actor", staleActorHandle}, {"world_snapshot_generation", 3}},
		store.Current());
	Require(
		!staleActor.Ok()
			&& staleActor.Error
			&& staleActor.Error->Code == "WORLD_ACTOR_HANDLE_STALE",
		"World actor detail accepted a stale serial for a reused object index");
	const auto levels = WorldCommandService::Execute(
		"world.levels",
		{{"cursor", nullptr}, {"limit", 128}},
		store.Current());
	Require(
		levels.Ok()
			&& levels.Data.at("levels").size() == 1
			&& levels.Data.at("levels").at(0).at("actor_count") == 3,
		"World level paging lost the captured actor count");
	const auto actors = WorldCommandService::Execute(
		"world.actors.list",
		{{"cursor", nullptr},
		 {"limit", 1},
		 {"search", "fixture"},
		 {"class_search", "actor"},
		 {"level_path", "/Game/Maps/Fixture.Fixture.PersistentLevel"}},
		store.Current());
	Require(
		actors.Ok()
			&& actors.Data.at("items").size() == 1
			&& actors.Data.at("matched") == 2
			&& actors.Data.at("has_more") == true
			&& actors.Data.at("items").at(0).at("handle").at("index") == 3
			&& actors.Data.at("items").at(0).at("level").at("handle").at("index") == 2,
		"World actor filtering did not retain exact actor/level handles");
	const nlohmann::json cursor = actors.Data.at("next_cursor");
	const auto actorsNext = WorldCommandService::Execute(
		"world.actors.list",
		{{"cursor", cursor},
		 {"limit", 1},
		 {"search", "fixture"},
		 {"class_search", "actor"},
		 {"level_path", "/Game/Maps/Fixture.Fixture.PersistentLevel"}},
		store.Current());
	Require(
		actorsNext.Ok()
			&& actorsNext.Data.at("items").size() == 1
			&& actorsNext.Data.at("items").at(0).at("handle").at("index") == 5
			&& actorsNext.Data.at("has_more") == false
			&& actorsNext.Data.at("next_cursor").is_null(),
		"World actor continuation cursor did not preserve the filtered result set");
	nlohmann::json terminalCursor = cursor;
	terminalCursor["after_ordinal"] = 2;
	const auto forgedTerminal = WorldCommandService::Execute(
		"world.actors.list",
		{{"cursor", terminalCursor},
		 {"limit", 1},
		 {"search", "fixture"},
		 {"class_search", "actor"},
		 {"level_path", "/Game/Maps/Fixture.Fixture.PersistentLevel"}},
		store.Current());
	Require(
		!forgedTerminal.Ok()
			&& forgedTerminal.Error
			&& forgedTerminal.Error->Code == "WORLD_CURSOR_INVALID",
		"World actor paging accepted a cursor that the service could never issue");

	store.Stop();
}

int main(const int argc, char** argv)
{
	try
	{
		if (argc == 2 && std::string_view(argv[1]) == "--host-session-fixture")
			return RunHostSessionFixture();
		if (argc != 2)
			throw std::runtime_error(
				"Usage: CoreHarness.exe <protocol-fixture-directory> | --host-session-fixture");
		const std::filesystem::path fixtureDirectory(argv[1]);
		TestGoldenHello(fixtureDirectory);
		TestMultipleFrames();
		TestTerminalErrors();
		TestFrameDecoderFuzzMatrix();
		TestUsmapContainer(fixtureDirectory);
		TestCoreSessionIdentity();
		TestEngineContextAndCapabilities();
		TestEngineNameCodec();
		TestPropertyCodec();
		TestWorldSnapshotQueries();
		TestReflectionLayout();
		TestCoreRuntimeStateAndShutdown();
		TestStableObjectAndFunctionHandles();
		TestProductionSnapshotMetadataSource();
		TestEngineFacadeAndImmutableSnapshots();
		TestObjectSnapshotReflectionCandidateSource();
		TestFPropertySnapshotReflectionCandidateSource();
		TestIncrementalSnapshotCapture();
		TestCoreDomainCommandsAndHandleExecution();
		TestNamedPipeRpcServerLifecycle();
		TestFUObjectItemIdentityLayout();
		TestBytePatternScanner();
		TestPeImageInspectionAndEngineVersionProbe();
		TestGlobalPointerDiscovery();
		TestHookOwnershipAndCallbackDrain();
		TestSafeMemory();
		TestQueueOwnershipAndBackpressure();
		TestQueueShutdownWakesWaiters();
		TestGameThreadTaskOwnershipAndTimeouts();
		TestGenericGameThreadWorkAndCancellation();
		TestPostRenderFrameClientOwnershipAndDrain();
		TestGameThreadFrameSchedulerBudgetFairnessAndDrain();
		TestGameThreadMpscCapacity();
		TestHttpServerLifecycle();
		std::cout << "Core harness passed: deterministic bounded frame fuzz/disconnect matrix, secure sessions, real current-user Windows Named Pipe RPC/event lifecycle, runtime/capabilities, EngineFacade/immutable budgeted object/type/world snapshots, domain commands, stable handles/FUObjectItem layout, budgeted witnessed reflection capture/layouts, bounded property codecs, bounded PE/version/global-pointer probing, pattern scanning, Hook RAII/drain, SafeMemory, USMAP consumer, bounded queues, cancellable owned game-thread/frame-client work, SEH, HTTP lifecycle, and shutdown.\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Core harness failed: " << error.what() << '\n';
		return 1;
	}
}
