#include "IPC/Protocol.h"
#include "Runtime/BoundedQueue.h"

#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
	using namespace UExplorer::IPC;

	void Require(const bool condition, const char* message)
	{
		if (!condition)
			throw std::runtime_error(message);
	}

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
		TestQueueOwnershipAndBackpressure();
		TestQueueShutdownWakesWaiters();
		std::cout << "Core harness passed: framing, queue ownership, backpressure, and shutdown wakeup.\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Core harness failed: " << error.what() << '\n';
		return 1;
	}
}
