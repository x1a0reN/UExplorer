#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include "IPC/Protocol.h"
#include "Runtime/BoundedQueue.h"
#include "API/GameThreadQueue.h"
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
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

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
		Require(cancelled.get() == SubmitResult::Disabled, "Disable did not wake the queued submitter");

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
			Require(result == SubmitResult::Disabled, "Shutdown did not cancel an MPSC producer task");
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
		TestQueueOwnershipAndBackpressure();
		TestQueueShutdownWakesWaiters();
		TestGameThreadTaskOwnershipAndTimeouts();
		TestGameThreadMpscCapacity();
		TestHttpServerLifecycle();
		std::cout << "Core harness passed: framing, bounded queues, owned game-thread tasks, SEH, HTTP lifecycle, and shutdown.\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Core harness failed: " << error.what() << '\n';
		return 1;
	}
}
