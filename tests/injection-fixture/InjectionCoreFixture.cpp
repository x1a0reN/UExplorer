#include <Windows.h>

#include "IPC/Protocol.h"
#include "Utils/Json/json.hpp"

#include <cstdint>
#include <string>
#include <vector>

#if !defined(UEXPLORER_FIXTURE_REJECT_LOAD)
namespace
{
	using UExplorer::IPC::DecodeHeader;
	using UExplorer::IPC::EncodeFrame;
	using UExplorer::IPC::FrameHeader;
	using UExplorer::IPC::FrameKind;
	using UExplorer::IPC::HeaderSize;
	using UExplorer::IPC::ProtocolError;

	bool ReadExact(const HANDLE pipe, void* output, const std::size_t size)
	{
		auto* cursor = static_cast<std::uint8_t*>(output);
		std::size_t remaining = size;
		while (remaining > 0)
		{
			DWORD read = 0;
			const DWORD chunk = static_cast<DWORD>((std::min)(remaining, static_cast<std::size_t>(64 * 1024)));
			if (!ReadFile(pipe, cursor, chunk, &read, nullptr) || read == 0)
				return false;
			cursor += read;
			remaining -= read;
		}
		return true;
	}

	bool WriteAll(const HANDLE pipe, const std::vector<std::uint8_t>& bytes)
	{
		std::size_t offset = 0;
		while (offset < bytes.size())
		{
			DWORD written = 0;
			const DWORD chunk = static_cast<DWORD>((std::min)(
				bytes.size() - offset,
				static_cast<std::size_t>(64 * 1024)));
			if (!WriteFile(pipe, bytes.data() + offset, chunk, &written, nullptr) || written == 0)
				return false;
			offset += written;
		}
		return true;
	}

	bool ReadFrame(const HANDLE pipe, FrameHeader& header, std::vector<std::uint8_t>& payload)
	{
		std::vector<std::uint8_t> headerBytes(HeaderSize);
		if (!ReadExact(pipe, headerBytes.data(), headerBytes.size()))
			return false;
		if (DecodeHeader(headerBytes, header) != ProtocolError::None)
			return false;
		payload.resize(header.PayloadLength);
		return payload.empty() || ReadExact(pipe, payload.data(), payload.size());
	}

	bool WriteJsonFrame(
		const HANDLE pipe,
		const FrameKind kind,
		const std::uint64_t requestId,
		const nlohmann::json& payload)
	{
		const std::string serialized = payload.dump();
		std::vector<std::uint8_t> frame;
		if (EncodeFrame(
			kind,
			requestId,
			std::span<const std::uint8_t>(
				reinterpret_cast<const std::uint8_t*>(serialized.data()),
				serialized.size()),
			frame) != ProtocolError::None)
		{
			return false;
		}
		return WriteAll(pipe, frame);
	}

	DWORD WINAPI RunFixtureServer(void*)
	{
		const DWORD processId = GetCurrentProcessId();
		const std::wstring pipeName = L"\\\\.\\pipe\\UExplorer\\v1\\" + std::to_wstring(processId);
		const HANDLE pipe = CreateNamedPipeW(
			pipeName.c_str(),
			PIPE_ACCESS_DUPLEX,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
			1,
			64 * 1024,
			64 * 1024,
			0,
			nullptr);
		if (pipe == INVALID_HANDLE_VALUE)
			return 1;

		const BOOL connected = ConnectNamedPipe(pipe, nullptr);
		if (!connected && GetLastError() != ERROR_PIPE_CONNECTED)
		{
			CloseHandle(pipe);
			return 2;
		}

		FrameHeader header;
		std::vector<std::uint8_t> payload;
		if (!ReadFrame(pipe, header, payload)
			|| header.Kind != FrameKind::Hello
			|| header.RequestId == 0)
		{
			CloseHandle(pipe);
			return 3;
		}

		const nlohmann::json hello = nlohmann::json::parse(payload, nullptr, false);
		if (hello.is_discarded()
			|| hello.value("target_pid", 0U) != processId
			|| hello.value("protocol", nlohmann::json::object()).value("major", 0U) != 1U
			|| hello.value("protocol", nlohmann::json::object()).value("minor", 1U) != 0U)
		{
			CloseHandle(pipe);
			return 4;
		}

		const std::string sessionId = "fixture-injected-" + std::to_string(processId);
		const nlohmann::json welcome = {
			{"core_version", "injection-fixture-1.0"},
			{"protocol", {{"major", 1}, {"minor", 0}}},
			{"session_id", sessionId},
			{"target_pid", processId},
			{"engine_profile", nullptr},
			{"capabilities", {
				{"engine.core", true},
				{"status.inspect", true},
				{"transport.named_pipe", true}
			}},
			{"limits", {
				{"max_payload_bytes", UExplorer::IPC::MaxPayloadSize},
				{"pending_rpc_per_session", 256},
				{"game_thread_tasks", 128},
				{"hook_event_ring", 8192},
				{"subscriber_events", 1024},
				{"dump_running", 1},
				{"max_timeout_ms", 120000}
			}}
		};
		if (!WriteJsonFrame(pipe, FrameKind::Welcome, header.RequestId, welcome))
		{
			CloseHandle(pipe);
			return 5;
		}

		while (ReadFrame(pipe, header, payload))
		{
			if (header.RequestId == 0)
				break;
			if (header.Kind == FrameKind::Ping)
			{
				std::vector<std::uint8_t> frame;
				if (EncodeFrame(FrameKind::Pong, header.RequestId, payload, frame) != ProtocolError::None
					|| !WriteAll(pipe, frame))
				{
					break;
				}
				continue;
			}
			if (header.Kind == FrameKind::Shutdown)
			{
				const nlohmann::json shutdown = nlohmann::json::parse(payload, nullptr, false);
				if (!shutdown.is_discarded()
					&& shutdown.value("session_id", std::string()) == sessionId
					&& WriteJsonFrame(pipe, FrameKind::Shutdown, header.RequestId, shutdown))
				{
					FlushFileBuffers(pipe);
				}
				break;
			}
		}

		DisconnectNamedPipe(pipe);
		CloseHandle(pipe);
		return 0;
	}
}
#endif

BOOL APIENTRY DllMain(const HMODULE module, const DWORD reason, void*)
{
	if (reason != DLL_PROCESS_ATTACH)
		return TRUE;
#if defined(UEXPLORER_FIXTURE_REJECT_LOAD)
	(void)module;
	return FALSE;
#else
	DisableThreadLibraryCalls(module);
#if defined(UEXPLORER_FIXTURE_DELAY_MS)
	Sleep(UEXPLORER_FIXTURE_DELAY_MS);
#endif
	const HANDLE worker = CreateThread(nullptr, 0, &RunFixtureServer, nullptr, 0, nullptr);
	if (worker == nullptr)
		return FALSE;
	CloseHandle(worker);
	return TRUE;
#endif
}
