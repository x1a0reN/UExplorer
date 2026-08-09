#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Sddl.h>

#include "IPC/NamedPipeRpcServer.h"

#include "IPC/Protocol.h"
#include "Utils/Json/json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace UExplorer::IPC
{
namespace
{
using json = nlohmann::json;

constexpr std::uint64_t kMaxProtocolInteger = 9'007'199'254'740'991ULL;
constexpr std::size_t kMaxVersionBytes = 64;
constexpr std::size_t kMaxReasonBytes = 256;
constexpr std::size_t kMaxErrorMessageBytes = 1024;
constexpr char kCoreVersion[] = "0.1.0";

bool IsBoundedText(const std::string& value, const std::size_t maximum)
{
	if (value.empty() || value.size() > maximum)
		return false;
	return std::none_of(value.begin(), value.end(), [](const unsigned char byte) {
		return byte < 0x20U || byte == 0x7FU;
	});
}

bool HasExactKeys(const json& value, const std::initializer_list<const char*> keys)
{
	if (!value.is_object() || value.size() != keys.size())
		return false;
	return std::all_of(keys.begin(), keys.end(), [&value](const char* key) {
		return value.contains(key);
	});
}

bool TryGetUnsigned(
	const json& value,
	const std::uint64_t maximum,
	std::uint64_t& output)
{
	if (!value.is_number_unsigned())
		return false;
	const std::uint64_t parsed = value.get<std::uint64_t>();
	if (parsed > maximum)
		return false;
	output = parsed;
	return true;
}

bool IsValidRequestId(const std::uint64_t requestId)
{
	return requestId > 0 && requestId <= kMaxProtocolInteger;
}

const char* ProtocolErrorCode(const ProtocolError error)
{
	switch (error)
	{
	case ProtocolError::None: return "NONE";
	case ProtocolError::IncompleteHeader: return "INCOMPLETE_HEADER";
	case ProtocolError::InvalidMagic: return "INVALID_MAGIC";
	case ProtocolError::UnsupportedMajor: return "UNSUPPORTED_MAJOR";
	case ProtocolError::UnknownKind: return "UNKNOWN_KIND";
	case ProtocolError::UnsupportedFlags: return "UNSUPPORTED_FLAGS";
	case ProtocolError::PayloadTooLarge: return "PAYLOAD_TOO_LARGE";
	case ProtocolError::InvalidPayloadLimit: return "INVALID_PAYLOAD_LIMIT";
	case ProtocolError::DecoderFailed: return "DECODER_FAILED";
	}
	return "UNKNOWN_PROTOCOL_ERROR";
}

Services::CoreCommandResponse FailureResponse(
	const std::uint64_t requestId,
	const std::string& sessionId,
	std::string code,
	std::string message,
	json details = json::object())
{
	return {
		.Ok = false,
		.RequestId = requestId,
		.SessionId = sessionId,
		.Data = nullptr,
		.Error = Services::CoreCommandError{
			.Code = std::move(code),
			.Message = std::move(message),
			.Details = details.is_object() ? std::move(details) : json::object()
		}
	};
}

json SerializeResponse(const Services::CoreCommandResponse& response)
{
	json error = nullptr;
	if (response.Error)
	{
		std::string message = response.Error->Message;
		if (!IsBoundedText(message, kMaxErrorMessageBytes))
			message = "Core command failed with an invalid or oversized diagnostic message";
		error = {
			{"code", response.Error->Code},
			{"message", std::move(message)},
			{"details", response.Error->Details.is_object()
				? response.Error->Details
				: json::object()}
		};
	}

	return {
		{"ok", response.Ok},
		{"request_id", response.RequestId},
		{"session_id", response.SessionId},
		{"error", std::move(error)},
		{"data", response.Ok ? response.Data : json(nullptr)},
		{"timing", {
			{"queued_us", response.Timing.QueuedUs},
			{"execute_us", response.Timing.ExecuteUs}
		}}
	};
}
}

class NamedPipeRpcServer::Impl final
{
public:
	Impl(
		Runtime::CoreRuntime& runtime,
		Services::CoreCommandService& commandService,
		Runtime::GameThreadExecutor& gameThread,
		ShutdownCallback shutdownCallback)
		: m_Runtime(runtime),
		  m_CommandService(commandService),
		  m_GameThread(gameThread),
		  m_ShutdownCallback(std::move(shutdownCallback)),
		  m_TargetProcessId(GetCurrentProcessId()),
		  m_PipeName(L"\\\\.\\pipe\\UExplorer\\v1\\" + std::to_wstring(m_TargetProcessId))
	{
	}

	~Impl()
	{
		if (!Stop(std::chrono::milliseconds(5000)))
			StopAndJoinWithoutTimeout();
		ReleaseNativeResources();
	}

	bool Start()
	{
		std::lock_guard<std::mutex> lifecycleLock(m_StartStopMutex);
		if (m_EverStarted || m_Started.load(std::memory_order_acquire))
			return false;
		m_EverStarted = true;

		const Runtime::CoreRuntimeSnapshot runtime = m_Runtime.Snapshot();
		if (!runtime.Context || runtime.Context->ProcessId() != m_TargetProcessId)
		{
			RecordError(
				"PIPE_TARGET_CONTEXT_MISMATCH",
				"EngineContext PID does not match the process hosting the named pipe",
				ERROR_INVALID_DATA);
			return false;
		}
		if (!m_CommandService.IsConfigured())
		{
			RecordError(
				"PIPE_COMMAND_SERVICE_NOT_CONFIGURED",
				"Named-pipe RPC cannot start without a configured Core command service",
				ERROR_INVALID_STATE);
			return false;
		}
		if (!BuildSecurityDescriptor())
			return false;

		m_StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!m_StopEvent)
		{
			RecordError("PIPE_STOP_EVENT_CREATE_FAILED", "Could not create the pipe stop event", GetLastError());
			return false;
		}

		m_BoundPipe = CreatePipeInstance();
		if (m_BoundPipe == INVALID_HANDLE_VALUE)
		{
			CloseHandle(m_StopEvent);
			m_StopEvent = nullptr;
			return false;
		}

		m_Started.store(true, std::memory_order_release);
		m_Listening.store(true, std::memory_order_release);
		try
		{
			m_Workers.reserve(kWorkerCount);
			for (std::size_t index = 0; index < kWorkerCount; ++index)
				m_Workers.emplace_back([this] { WorkerLoop(); });
			m_Listener = std::thread([this] { ListenerLoop(); });
		}
		catch (...)
		{
			RecordError(
				"PIPE_THREAD_START_FAILED",
				"Could not create all joinable named-pipe threads",
				ERROR_NOT_ENOUGH_MEMORY);
			RequestStop();
			if (m_BoundPipe != INVALID_HANDLE_VALUE)
			{
				CancelIoEx(m_BoundPipe, nullptr);
				CloseHandle(m_BoundPipe);
				m_BoundPipe = INVALID_HANDLE_VALUE;
			}
			for (std::thread& worker : m_Workers)
			{
				if (worker.joinable())
					worker.join();
			}
			m_Workers.clear();
			m_Started.store(false, std::memory_order_release);
			m_Listening.store(false, std::memory_order_release);
			return false;
		}
		return true;
	}

	bool OpenAdmissions()
	{
		if (!m_Started.load(std::memory_order_acquire)
			|| !m_Listening.load(std::memory_order_acquire)
			|| m_StopRequested.load(std::memory_order_acquire))
		{
			return false;
		}

		const Runtime::CoreRuntimeSnapshot runtime = m_Runtime.Snapshot();
		if (!runtime.IsReady()
			|| !runtime.Context
			|| runtime.Context->ProcessId() != m_TargetProcessId
			|| !runtime.Capabilities
			|| !runtime.Capabilities->IsAvailable("transport.named_pipe"))
		{
			return false;
		}

		m_AdmissionsOpen.store(true, std::memory_order_release);
		m_AdmissionsCondition.notify_all();
		return true;
	}

	bool Stop(const std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lifecycleLock(m_StartStopMutex);
		if (!m_Started.load(std::memory_order_acquire))
			return true;

		RequestStop();
		CancelAllPending();
		lifecycleLock.unlock();

		const bool exited = WaitForOwnedThreads(timeout);
		if (!exited)
		{
			RecordError(
				"PIPE_STOP_TIMEOUT",
				"Named-pipe listener or request workers did not drain before the stop deadline",
				WAIT_TIMEOUT);
			return false;
		}

		lifecycleLock.lock();
		JoinOwnedThreads();
		m_Started.store(false, std::memory_order_release);
		m_Listening.store(false, std::memory_order_release);
		m_ConnectedClientProcessId.store(0, std::memory_order_release);
		return true;
	}

	bool IsListening() const noexcept
	{
		return m_Started.load(std::memory_order_acquire)
			&& m_Listening.load(std::memory_order_acquire)
			&& !m_StopRequested.load(std::memory_order_acquire);
	}

	std::wstring PipeName() const
	{
		return m_PipeName;
	}

	NamedPipeServerDiagnostics Diagnostics() const
	{
		NamedPipeServerDiagnostics diagnostics;
		diagnostics.Started = m_Started.load(std::memory_order_acquire);
		diagnostics.Listening = m_Listening.load(std::memory_order_acquire);
		diagnostics.AdmissionsOpen = m_AdmissionsOpen.load(std::memory_order_acquire);
		diagnostics.StopRequested = m_StopRequested.load(std::memory_order_acquire);
		diagnostics.TargetProcessId = m_TargetProcessId;
		diagnostics.ConnectedClientProcessId =
			m_ConnectedClientProcessId.load(std::memory_order_acquire);
		diagnostics.PendingRequests = m_PendingCount.load(std::memory_order_acquire);
		diagnostics.QueuedRequests = m_QueuedCount.load(std::memory_order_acquire);
		diagnostics.WorkerCount = m_LiveWorkers.load(std::memory_order_acquire);
		diagnostics.AcceptedConnections = m_AcceptedConnections.load(std::memory_order_acquire);
		diagnostics.RejectedConnections = m_RejectedConnections.load(std::memory_order_acquire);
		diagnostics.CompletedRequests = m_CompletedRequests.load(std::memory_order_acquire);
		diagnostics.CancelledRequests = m_CancelledRequests.load(std::memory_order_acquire);
		{
			std::lock_guard<std::mutex> lock(m_ErrorMutex);
			diagnostics.LastNativeError = m_LastNativeError;
			diagnostics.LastErrorCode = m_LastErrorCode;
			diagnostics.LastErrorMessage = m_LastErrorMessage;
		}
		return diagnostics;
	}

private:
	struct Connection final
	{
		std::uint64_t Id = 0;
		HANDLE Pipe = INVALID_HANDLE_VALUE;
		std::uint32_t ClientProcessId = 0;
		std::atomic<bool> Active{true};
		std::atomic<bool> GracefulClose{false};
		std::atomic<bool> ShutdownRequested{false};
		std::mutex WriteMutex;
	};

	struct RequestKey final
	{
		std::uint64_t ConnectionId = 0;
		std::uint64_t RequestId = 0;

		bool operator==(const RequestKey&) const = default;
	};

	struct RequestKeyHash final
	{
		std::size_t operator()(const RequestKey& key) const noexcept
		{
			const std::size_t first = std::hash<std::uint64_t>{}(key.ConnectionId);
			const std::size_t second = std::hash<std::uint64_t>{}(key.RequestId);
			return first ^ (second + 0x9E3779B9U + (first << 6U) + (first >> 2U));
		}
	};

	struct PendingRequest final
	{
		RequestKey Key;
		std::shared_ptr<Connection> Client;
		Services::CoreCommandRequest Command;
		std::chrono::steady_clock::time_point ReceivedAt;
		std::chrono::steady_clock::time_point Deadline;
		std::atomic<bool> CancelRequested{false};
		std::atomic<bool> Started{false};
		std::atomic<bool> CancellationCounted{false};
		std::mutex TicketMutex;
		std::optional<Runtime::GameThreadTicket> Ticket;
	};

	void RecordError(std::string code, std::string message, const DWORD nativeError)
	{
		std::lock_guard<std::mutex> lock(m_ErrorMutex);
		m_LastNativeError = nativeError;
		m_LastErrorCode = std::move(code);
		m_LastErrorMessage = std::move(message);
	}

	bool BuildSecurityDescriptor()
	{
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
		{
			RecordError("PIPE_TOKEN_OPEN_FAILED", "Could not open the Core process token", GetLastError());
			return false;
		}

		DWORD required = 0;
		GetTokenInformation(token, TokenUser, nullptr, 0, &required);
		const DWORD sizeError = GetLastError();
		if (required == 0 || sizeError != ERROR_INSUFFICIENT_BUFFER)
		{
			CloseHandle(token);
			RecordError("PIPE_TOKEN_USER_QUERY_FAILED", "Could not size the Core token user identity", sizeError);
			return false;
		}

		std::vector<std::uint8_t> tokenUserBytes(required);
		if (!GetTokenInformation(token, TokenUser, tokenUserBytes.data(), required, &required))
		{
			const DWORD error = GetLastError();
			CloseHandle(token);
			RecordError("PIPE_TOKEN_USER_QUERY_FAILED", "Could not read the Core token user identity", error);
			return false;
		}
		CloseHandle(token);

		const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(tokenUserBytes.data());
		const DWORD sidBytes = GetLengthSid(tokenUser->User.Sid);
		m_CurrentUserSid.resize(sidBytes);
		if (!CopySid(sidBytes, m_CurrentUserSid.data(), tokenUser->User.Sid))
		{
			RecordError("PIPE_SID_COPY_FAILED", "Could not copy the Core user SID", GetLastError());
			return false;
		}

		LPWSTR sidText = nullptr;
		if (!ConvertSidToStringSidW(m_CurrentUserSid.data(), &sidText))
		{
			RecordError("PIPE_SID_FORMAT_FAILED", "Could not format the Core user SID", GetLastError());
			return false;
		}
		const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sidText) + L")";
		LocalFree(sidText);

		if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
			sddl.c_str(),
			SDDL_REVISION_1,
			&m_SecurityDescriptor,
			nullptr))
		{
			RecordError("PIPE_DACL_CREATE_FAILED", "Could not create the current-user-only pipe DACL", GetLastError());
			return false;
		}

		m_SecurityAttributes.nLength = sizeof(m_SecurityAttributes);
		m_SecurityAttributes.lpSecurityDescriptor = m_SecurityDescriptor;
		m_SecurityAttributes.bInheritHandle = FALSE;
		return true;
	}

	HANDLE CreatePipeInstance()
	{
		HANDLE pipe = CreateNamedPipeW(
			m_PipeName.c_str(),
			PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
			1,
			static_cast<DWORD>(kIoChunkBytes),
			static_cast<DWORD>(kIoChunkBytes),
			0,
			&m_SecurityAttributes);
		if (pipe == INVALID_HANDLE_VALUE)
			RecordError("PIPE_CREATE_FAILED", "Could not bind the canonical target-PID pipe name", GetLastError());
		return pipe;
	}

	void ListenerLoop() noexcept
	{
		HANDLE pipe = INVALID_HANDLE_VALUE;
		{
			std::lock_guard<std::mutex> lock(m_BoundPipeMutex);
			pipe = std::exchange(m_BoundPipe, INVALID_HANDLE_VALUE);
		}

		while (!m_StopRequested.load(std::memory_order_acquire))
		{
			if (pipe == INVALID_HANDLE_VALUE)
			{
				pipe = CreatePipeInstance();
				if (pipe == INVALID_HANDLE_VALUE)
				{
					m_Listening.store(false, std::memory_order_release);
					RequestStop();
					break;
				}
			}

			if (!ConnectClient(pipe))
			{
				DisconnectNamedPipe(pipe);
				CloseHandle(pipe);
				pipe = INVALID_HANDLE_VALUE;
				if (!m_StopRequested.load(std::memory_order_acquire))
					m_RejectedConnections.fetch_add(1, std::memory_order_relaxed);
				continue;
			}

			auto connection = std::make_shared<Connection>();
			connection->Id = m_NextConnectionId.fetch_add(1, std::memory_order_relaxed);
			connection->Pipe = pipe;
			pipe = INVALID_HANDLE_VALUE;
			{
				std::lock_guard<std::mutex> lock(m_CurrentConnectionMutex);
				m_CurrentConnection = connection;
			}

			RunConnection(connection);

			CancelConnectionPending(connection->Id);
			const bool acknowledgementDrained = CloseConnection(*connection);
			if (connection->ShutdownRequested.load(std::memory_order_acquire)
				&& acknowledgementDrained)
				InvokeShutdownCallback();
			{
				std::lock_guard<std::mutex> lock(m_CurrentConnectionMutex);
				if (m_CurrentConnection == connection)
					m_CurrentConnection.reset();
			}
			m_ConnectedClientProcessId.store(0, std::memory_order_release);
		}

		if (pipe != INVALID_HANDLE_VALUE)
		{
			CancelIoEx(pipe, nullptr);
			DisconnectNamedPipe(pipe);
			CloseHandle(pipe);
		}
		m_Listening.store(false, std::memory_order_release);
		m_ListenerExited.store(true, std::memory_order_release);
		m_LifecycleCondition.notify_all();
	}

	bool ConnectClient(const HANDLE pipe)
	{
		OVERLAPPED operation{};
		operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!operation.hEvent)
		{
			RecordError("PIPE_CONNECT_EVENT_FAILED", "Could not create an overlapped connect event", GetLastError());
			return false;
		}

		bool connected = false;
		if (ConnectNamedPipe(pipe, &operation))
		{
			connected = true;
		}
		else
		{
			const DWORD error = GetLastError();
			if (error == ERROR_PIPE_CONNECTED)
			{
				connected = true;
			}
			else if (error == ERROR_IO_PENDING)
			{
				const std::array<HANDLE, 2> waits{operation.hEvent, m_StopEvent};
				const DWORD wait = WaitForMultipleObjects(
					static_cast<DWORD>(waits.size()), waits.data(), FALSE, INFINITE);
				if (wait == WAIT_OBJECT_0)
				{
					DWORD transferred = 0;
					connected = GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE;
				}
				else if (wait == WAIT_OBJECT_0 + 1)
				{
					CancelIoEx(pipe, &operation);
					WaitForSingleObject(operation.hEvent, INFINITE);
				}
				else
				{
					RecordError("PIPE_CONNECT_WAIT_FAILED", "Named-pipe connect wait failed", GetLastError());
				}
			}
			else
			{
				RecordError("PIPE_CONNECT_FAILED", "Named-pipe client connection failed", error);
			}
		}

		CloseHandle(operation.hEvent);
		return connected && !m_StopRequested.load(std::memory_order_acquire);
	}

	bool VerifyClientIdentity(Connection& connection)
	{
		ULONG clientProcessId = 0;
		if (!GetNamedPipeClientProcessId(connection.Pipe, &clientProcessId) || clientProcessId == 0)
		{
			RecordError("PIPE_CLIENT_PID_QUERY_FAILED", "Could not identify the named-pipe client process", GetLastError());
			return false;
		}

		if (!ImpersonateNamedPipeClient(connection.Pipe))
		{
			RecordError("PIPE_CLIENT_IMPERSONATION_FAILED", "Could not verify the named-pipe client token", GetLastError());
			return false;
		}

		bool matches = false;
		DWORD failure = ERROR_SUCCESS;
		HANDLE token = nullptr;
		if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token))
		{
			failure = GetLastError();
		}
		else
		{
			DWORD required = 0;
			GetTokenInformation(token, TokenUser, nullptr, 0, &required);
			failure = GetLastError();
			if (required > 0 && failure == ERROR_INSUFFICIENT_BUFFER)
			{
				std::vector<std::uint8_t> bytes(required);
				if (GetTokenInformation(token, TokenUser, bytes.data(), required, &required))
				{
					const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(bytes.data());
					matches = EqualSid(m_CurrentUserSid.data(), tokenUser->User.Sid) != FALSE;
					failure = matches ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
				}
				else
				{
					failure = GetLastError();
				}
			}
			CloseHandle(token);
		}

		if (!RevertToSelf())
		{
			RecordError("PIPE_CLIENT_REVERT_FAILED", "Could not leave named-pipe client impersonation", GetLastError());
			return false;
		}
		if (!matches)
		{
			RecordError("PIPE_CLIENT_IDENTITY_REJECTED", "Named-pipe client SID does not match the Core process user", failure);
			return false;
		}

		connection.ClientProcessId = static_cast<std::uint32_t>(clientProcessId);
		m_ConnectedClientProcessId.store(connection.ClientProcessId, std::memory_order_release);
		return true;
	}

	void RunConnection(const std::shared_ptr<Connection>& connection)
	{
		{
			std::unique_lock<std::mutex> lock(m_AdmissionsMutex);
			m_AdmissionsCondition.wait(lock, [this] {
				return m_AdmissionsOpen.load(std::memory_order_acquire)
					|| m_StopRequested.load(std::memory_order_acquire);
			});
		}
		if (m_StopRequested.load(std::memory_order_acquire))
			return;

		Frame helloFrame;
		if (!ReadFrame(*connection, helloFrame)
			|| !VerifyClientIdentity(*connection)
			|| !HandleHello(connection, helloFrame))
		{
			m_RejectedConnections.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		m_AcceptedConnections.fetch_add(1, std::memory_order_relaxed);

		while (connection->Active.load(std::memory_order_acquire)
			&& !m_StopRequested.load(std::memory_order_acquire))
		{
			Frame frame;
			if (!ReadFrame(*connection, frame))
				break;
			if (!HandleSessionFrame(connection, frame))
				break;
		}
	}

	bool HandleHello(const std::shared_ptr<Connection>& connection, const Frame& frame)
	{
		if (frame.Header.Kind != FrameKind::Hello
			|| frame.Header.Minor != ProtocolMinor
			|| !IsValidRequestId(frame.Header.RequestId))
		{
			RecordError("PIPE_HANDSHAKE_INVALID", "Connection did not begin with one correlated v1 Hello", ERROR_INVALID_DATA);
			return false;
		}

		const std::optional<json> payload = ParsePayload(frame.Payload);
		if (!payload
			|| !HasExactKeys(*payload, {"host_version", "protocol", "target_pid"})
			|| !payload->at("host_version").is_string()
			|| !HasExactKeys(payload->at("protocol"), {"major", "minor"}))
		{
			RecordError("PIPE_HELLO_PAYLOAD_INVALID", "Hello payload does not match the closed v1 schema", ERROR_INVALID_DATA);
			return false;
		}

		std::uint64_t major = 0;
		std::uint64_t minor = 0;
		std::uint64_t targetPid = 0;
		const std::string hostVersion = payload->at("host_version").get<std::string>();
		if (!IsBoundedText(hostVersion, kMaxVersionBytes)
			|| !TryGetUnsigned(payload->at("protocol").at("major"), ProtocolMajor, major)
			|| !TryGetUnsigned(payload->at("protocol").at("minor"), ProtocolMinor, minor)
			|| major != ProtocolMajor
			|| minor != ProtocolMinor
			|| !TryGetUnsigned(payload->at("target_pid"), (std::numeric_limits<std::uint32_t>::max)(), targetPid)
			|| targetPid != m_TargetProcessId)
		{
			RecordError("PIPE_HELLO_IDENTITY_MISMATCH", "Hello version or target PID is invalid", ERROR_INVALID_DATA);
			return false;
		}

		const Runtime::CoreRuntimeSnapshot runtime = m_Runtime.Snapshot();
		if (!runtime.IsReady() || !runtime.Context || !runtime.Capabilities
			|| runtime.Context->ProcessId() != m_TargetProcessId
			|| !runtime.Capabilities->IsAvailable("engine.core")
			|| !runtime.Capabilities->IsAvailable("transport.named_pipe"))
		{
			RecordError("PIPE_CORE_NOT_READY", "CoreRuntime lost readiness before Welcome publication", ERROR_INVALID_STATE);
			return false;
		}

		json capabilities = json::object();
		for (const auto& [name, capability] : runtime.Capabilities->All())
			capabilities[name] = capability.Available;

		const json welcome = {
			{"core_version", kCoreVersion},
			{"protocol", {{"major", ProtocolMajor}, {"minor", ProtocolMinor}}},
			{"session_id", runtime.SessionId},
			{"target_pid", m_TargetProcessId},
			{"engine_profile", nullptr},
			{"capabilities", std::move(capabilities)},
			{"limits", {
				{"max_payload_bytes", MaxPayloadSize},
				{"pending_rpc_per_session", kPendingRequestLimit},
				{"game_thread_tasks", Runtime::GameThreadExecutor::kCapacity},
				{"hook_event_ring", 8192},
				{"subscriber_events", 1024},
				{"dump_running", 1},
				{"max_timeout_ms", Runtime::GameThreadExecutor::kMaxTimeoutMs}
			}}
		};
		return WriteJsonFrame(*connection, FrameKind::Welcome, frame.Header.RequestId, welcome);
	}

	bool HandleSessionFrame(const std::shared_ptr<Connection>& connection, const Frame& frame)
	{
		if (frame.Header.Minor != ProtocolMinor || !IsValidRequestId(frame.Header.RequestId))
		{
			RecordError("PIPE_SESSION_ENVELOPE_INVALID", "Session frame version or request ID is invalid", ERROR_INVALID_DATA);
			return false;
		}

		switch (frame.Header.Kind)
		{
		case FrameKind::Request:
			return HandleRequest(connection, frame);
		case FrameKind::Cancel:
			return HandleCancel(connection, frame);
		case FrameKind::Ping:
			return HandlePing(*connection, frame);
		case FrameKind::Shutdown:
			return HandleShutdown(*connection, frame);
		default:
			RecordError("PIPE_FRAME_KIND_INVALID", "Host sent a frame kind that is invalid for a Core session", ERROR_INVALID_DATA);
			return false;
		}
	}

	bool HandleRequest(const std::shared_ptr<Connection>& connection, const Frame& frame)
	{
		const std::optional<json> payload = ParsePayload(frame.Payload);
		if (!payload
			|| !HasExactKeys(*payload, {"operation", "session_id", "timeout_ms", "data"})
			|| !payload->at("operation").is_string()
			|| !payload->at("session_id").is_string())
		{
			RecordError("PIPE_REQUEST_PAYLOAD_INVALID", "Request payload does not match the closed v1 schema", ERROR_INVALID_DATA);
			return false;
		}

		std::uint64_t timeoutMs = 0;
		const std::string operation = payload->at("operation").get<std::string>();
		const std::string sessionId = payload->at("session_id").get<std::string>();
		if (sessionId != m_CommandService.SessionId()
			|| !TryGetUnsigned(
				payload->at("timeout_ms"),
				Runtime::GameThreadExecutor::kMaxTimeoutMs,
				timeoutMs)
			|| timeoutMs == 0)
		{
			RecordError("PIPE_REQUEST_SESSION_OR_TIMEOUT_INVALID", "Request session or timeout is invalid", ERROR_INVALID_DATA);
			return false;
		}

		auto pending = std::make_shared<PendingRequest>();
		pending->Key = {.ConnectionId = connection->Id, .RequestId = frame.Header.RequestId};
		pending->Client = connection;
		pending->Command = {
			.RequestId = frame.Header.RequestId,
			.Operation = operation,
			.SessionId = sessionId,
			.TimeoutMs = static_cast<int>(timeoutMs),
			.Data = payload->at("data")
		};
		pending->ReceivedAt = std::chrono::steady_clock::now();
		pending->Deadline = pending->ReceivedAt + std::chrono::milliseconds(timeoutMs);

		bool saturated = false;
		{
			std::lock_guard<std::mutex> lock(m_JobsMutex);
			if (m_Pending.contains(pending->Key))
			{
				RecordError("PIPE_REQUEST_ID_DUPLICATE", "Request ID is already pending in this session", ERROR_INVALID_DATA);
				return false;
			}
			if (m_Pending.size() >= kPendingRequestLimit)
			{
				saturated = true;
			}
			else
			{
				m_Pending.emplace(pending->Key, pending);
				m_Jobs.push_back(pending);
				m_PendingCount.store(m_Pending.size(), std::memory_order_release);
				m_QueuedCount.store(m_Jobs.size(), std::memory_order_release);
			}
		}
		if (saturated)
		{
			return WriteResponse(
				*connection,
				FailureResponse(
					frame.Header.RequestId,
					sessionId,
					"RPC_BACKPRESSURE",
					"The per-session pending RPC limit is exhausted"));
		}
		m_JobsCondition.notify_one();
		return true;
	}

	bool HandleCancel(const std::shared_ptr<Connection>& connection, const Frame& frame)
	{
		const std::optional<json> payload = ParsePayload(frame.Payload);
		if (!payload
			|| !HasExactKeys(*payload, {"session_id", "reason"})
			|| !payload->at("session_id").is_string()
			|| !payload->at("reason").is_string())
		{
			RecordError("PIPE_CANCEL_PAYLOAD_INVALID", "Cancel payload does not match the closed v1 schema", ERROR_INVALID_DATA);
			return false;
		}
		const std::string sessionId = payload->at("session_id").get<std::string>();
		const std::string reason = payload->at("reason").get<std::string>();
		if (sessionId != m_CommandService.SessionId() || !IsBoundedText(reason, kMaxReasonBytes))
		{
			RecordError("PIPE_CANCEL_IDENTITY_INVALID", "Cancel session or reason is invalid", ERROR_INVALID_DATA);
			return false;
		}

		std::shared_ptr<PendingRequest> pending;
		{
			std::lock_guard<std::mutex> lock(m_JobsMutex);
			const auto it = m_Pending.find({.ConnectionId = connection->Id, .RequestId = frame.Header.RequestId});
			if (it != m_Pending.end())
				pending = it->second;
		}
		if (!pending)
			return true;

		CancelPending(*pending);
		return true;
	}

	bool HandlePing(Connection& connection, const Frame& frame)
	{
		const std::optional<json> payload = ParsePayload(frame.Payload);
		if (!payload
			|| !HasExactKeys(*payload, {"session_id", "nonce", "sent_at_monotonic_us"})
			|| !payload->at("session_id").is_string())
		{
			RecordError("PIPE_HEARTBEAT_PAYLOAD_INVALID", "Ping payload does not match the closed v1 schema", ERROR_INVALID_DATA);
			return false;
		}
		std::uint64_t nonce = 0;
		std::uint64_t sentAt = 0;
		if (payload->at("session_id").get<std::string>() != m_CommandService.SessionId()
			|| !TryGetUnsigned(payload->at("nonce"), kMaxProtocolInteger, nonce)
			|| !TryGetUnsigned(payload->at("sent_at_monotonic_us"), kMaxProtocolInteger, sentAt)
			|| nonce == 0
			|| sentAt == 0)
		{
			RecordError("PIPE_HEARTBEAT_INVALID", "Ping session, nonce, or timestamp is invalid", ERROR_INVALID_DATA);
			return false;
		}
		return WriteJsonFrame(connection, FrameKind::Pong, frame.Header.RequestId, *payload);
	}

	bool HandleShutdown(Connection& connection, const Frame& frame)
	{
		const std::optional<json> payload = ParsePayload(frame.Payload);
		if (!payload
			|| !HasExactKeys(*payload, {"session_id", "reason"})
			|| !payload->at("session_id").is_string()
			|| !payload->at("reason").is_string())
		{
			RecordError("PIPE_SHUTDOWN_PAYLOAD_INVALID", "Shutdown payload does not match the closed v1 schema", ERROR_INVALID_DATA);
			return false;
		}
		const std::string sessionId = payload->at("session_id").get<std::string>();
		const std::string reason = payload->at("reason").get<std::string>();
		if (sessionId != m_CommandService.SessionId() || !IsBoundedText(reason, kMaxReasonBytes))
		{
			RecordError("PIPE_SHUTDOWN_IDENTITY_INVALID", "Shutdown session or reason is invalid", ERROR_INVALID_DATA);
			return false;
		}
		if (!WriteJsonFrame(connection, FrameKind::Shutdown, frame.Header.RequestId, *payload))
			return false;

		connection.GracefulClose.store(true, std::memory_order_release);
		connection.ShutdownRequested.store(true, std::memory_order_release);
		connection.Active.store(false, std::memory_order_release);
		return false;
	}

	void InvokeShutdownCallback()
	{
		if (m_ShutdownCallbackInvoked.exchange(true, std::memory_order_acq_rel)
			|| !m_ShutdownCallback)
		{
			return;
		}
		try
		{
			m_ShutdownCallback();
		}
		catch (...)
		{
			RecordError("PIPE_SHUTDOWN_CALLBACK_FAILED", "Host shutdown callback raised an exception", ERROR_UNHANDLED_EXCEPTION);
		}
	}

	std::optional<json> ParsePayload(const std::vector<std::uint8_t>& bytes)
	{
		try
		{
			json payload = json::parse(bytes.begin(), bytes.end(), nullptr, false);
			if (payload.is_discarded())
				return std::nullopt;
			return payload;
		}
		catch (...)
		{
			return std::nullopt;
		}
	}

	bool ReadFrame(Connection& connection, Frame& output)
	{
		std::array<std::uint8_t, HeaderSize> headerBytes{};
		if (!ReadExact(connection.Pipe, headerBytes.data(), headerBytes.size()))
			return false;

		const ProtocolError error = DecodeHeader(headerBytes, output.Header);
		if (error != ProtocolError::None)
		{
			RecordError(
				"PIPE_PROTOCOL_" + std::string(ProtocolErrorCode(error)),
				"Named-pipe frame header violates the v1 contract",
				ERROR_INVALID_DATA);
			return false;
		}

		output.Payload.resize(output.Header.PayloadLength);
		if (!output.Payload.empty()
			&& !ReadExact(connection.Pipe, output.Payload.data(), output.Payload.size()))
		{
			output.Payload.clear();
			return false;
		}
		return true;
	}

	bool ReadExact(const HANDLE pipe, std::uint8_t* output, const std::size_t size)
	{
		std::size_t offset = 0;
		while (offset < size)
		{
			const DWORD chunk = static_cast<DWORD>((std::min)(size - offset, kIoChunkBytes));
			DWORD transferred = 0;
			if (!RunOverlappedIo(pipe, output + offset, chunk, false, transferred) || transferred == 0)
				return false;
			offset += transferred;
		}
		return true;
	}

	bool WriteExact(const HANDLE pipe, const std::uint8_t* bytes, const std::size_t size)
	{
		std::size_t offset = 0;
		while (offset < size)
		{
			const DWORD chunk = static_cast<DWORD>((std::min)(size - offset, kIoChunkBytes));
			DWORD transferred = 0;
			if (!RunOverlappedIo(
				pipe,
				const_cast<std::uint8_t*>(bytes + offset),
				chunk,
				true,
				transferred)
				|| transferred == 0)
			{
				return false;
			}
			offset += transferred;
		}
		return true;
	}

	bool RunOverlappedIo(
		const HANDLE pipe,
		void* bytes,
		const DWORD size,
		const bool write,
		DWORD& transferred)
	{
		OVERLAPPED operation{};
		operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!operation.hEvent)
		{
			RecordError("PIPE_IO_EVENT_FAILED", "Could not create an overlapped pipe I/O event", GetLastError());
			return false;
		}

		const BOOL started = write
			? WriteFile(pipe, bytes, size, &transferred, &operation)
			: ReadFile(pipe, bytes, size, &transferred, &operation);
		bool completed = started != FALSE;
		if (!completed)
		{
			const DWORD error = GetLastError();
			if (error == ERROR_IO_PENDING)
			{
				const std::array<HANDLE, 2> waits{operation.hEvent, m_StopEvent};
				const DWORD wait = WaitForMultipleObjects(
					static_cast<DWORD>(waits.size()), waits.data(), FALSE, INFINITE);
				if (wait == WAIT_OBJECT_0)
				{
					completed = GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE;
				}
				else if (wait == WAIT_OBJECT_0 + 1)
				{
					CancelIoEx(pipe, &operation);
					WaitForSingleObject(operation.hEvent, INFINITE);
				}
			}
		}

		CloseHandle(operation.hEvent);
		return completed;
	}

	bool WriteJsonFrame(
		Connection& connection,
		const FrameKind kind,
		const std::uint64_t requestId,
		const json& payload)
	{
		std::string serialized;
		try
		{
			serialized = payload.dump();
		}
		catch (...)
		{
			RecordError("PIPE_JSON_SERIALIZE_FAILED", "Could not serialize an RPC payload", ERROR_INVALID_DATA);
			return false;
		}

		const auto payloadBytes = std::span<const std::uint8_t>(
			reinterpret_cast<const std::uint8_t*>(serialized.data()),
			serialized.size());
		std::vector<std::uint8_t> frame;
		if (EncodeFrame(kind, requestId, payloadBytes, frame) != ProtocolError::None)
		{
			RecordError("PIPE_RESPONSE_TOO_LARGE", "RPC payload exceeds the v1 frame limit", ERROR_BUFFER_OVERFLOW);
			return false;
		}

		std::lock_guard<std::mutex> lock(connection.WriteMutex);
		if (!connection.Active.load(std::memory_order_acquire)
			|| connection.Pipe == INVALID_HANDLE_VALUE)
		{
			return false;
		}
		if (!WriteExact(connection.Pipe, frame.data(), frame.size()))
		{
			connection.Active.store(false, std::memory_order_release);
			return false;
		}
		return true;
	}

	bool WriteResponse(Connection& connection, const Services::CoreCommandResponse& response)
	{
		return WriteJsonFrame(
			connection,
			FrameKind::Response,
			response.RequestId,
			SerializeResponse(response));
	}

	void WorkerLoop() noexcept
	{
		m_LiveWorkers.fetch_add(1, std::memory_order_relaxed);
		for (;;)
		{
			std::shared_ptr<PendingRequest> pending;
			{
				std::unique_lock<std::mutex> lock(m_JobsMutex);
				m_JobsCondition.wait(lock, [this] {
					return m_StopRequested.load(std::memory_order_acquire) || !m_Jobs.empty();
				});
				if (m_Jobs.empty())
				{
					if (m_StopRequested.load(std::memory_order_acquire))
						break;
					continue;
				}
				pending = m_Jobs.front();
				m_Jobs.pop_front();
				m_QueuedCount.store(m_Jobs.size(), std::memory_order_release);
			}

			pending->Started.store(true, std::memory_order_release);
			Services::CoreCommandResponse response;
			const auto now = std::chrono::steady_clock::now();
			const auto queuedDuration = std::chrono::duration_cast<std::chrono::microseconds>(
				now - pending->ReceivedAt).count();
			const std::uint64_t transportQueuedUs = queuedDuration <= 0
				? 0
				: static_cast<std::uint64_t>(queuedDuration);
			if (pending->CancelRequested.load(std::memory_order_acquire)
				|| m_StopRequested.load(std::memory_order_acquire))
			{
				response = FailureResponse(
					pending->Command.RequestId,
					pending->Command.SessionId,
					"REQUEST_CANCELLED",
					"Request was cancelled before execution");
			}
			else if (now >= pending->Deadline)
			{
				response = FailureResponse(
					pending->Command.RequestId,
					pending->Command.SessionId,
					"DEADLINE_EXPIRED",
					"Request deadline expired before execution");
			}
			else
			{
				const auto remainingUs = std::chrono::duration_cast<std::chrono::microseconds>(
					pending->Deadline - now).count();
				pending->Command.TimeoutMs = static_cast<int>((std::max<std::int64_t>)(
					1,
					(remainingUs + 999) / 1000));
				response = m_CommandService.Execute(
					pending->Command,
					[this, pending](const Runtime::GameThreadTicket& ticket) {
						{
							std::lock_guard<std::mutex> lock(pending->TicketMutex);
							pending->Ticket = ticket;
						}
						if (pending->CancelRequested.load(std::memory_order_acquire)
							|| m_StopRequested.load(std::memory_order_acquire))
						{
							m_GameThread.Cancel(ticket);
						}
					});
			}

			const std::uint64_t available = (std::numeric_limits<std::uint64_t>::max)()
				- response.Timing.QueuedUs;
			response.Timing.QueuedUs += (std::min)(available, transportQueuedUs);
			if (pending->Client->Active.load(std::memory_order_acquire)
				&& !m_StopRequested.load(std::memory_order_acquire))
			{
				WriteResponse(*pending->Client, response);
			}

			{
				std::lock_guard<std::mutex> lock(m_JobsMutex);
				m_Pending.erase(pending->Key);
				m_PendingCount.store(m_Pending.size(), std::memory_order_release);
			}
			m_CompletedRequests.fetch_add(1, std::memory_order_relaxed);
		}

		m_LiveWorkers.fetch_sub(1, std::memory_order_relaxed);
		m_WorkersExited.fetch_add(1, std::memory_order_release);
		m_LifecycleCondition.notify_all();
	}

	void CancelPending(PendingRequest& pending)
	{
		pending.CancelRequested.store(true, std::memory_order_release);
		if (!pending.CancellationCounted.exchange(true, std::memory_order_acq_rel))
			m_CancelledRequests.fetch_add(1, std::memory_order_relaxed);

		std::optional<Runtime::GameThreadTicket> ticket;
		{
			std::lock_guard<std::mutex> lock(pending.TicketMutex);
			ticket = pending.Ticket;
		}
		if (ticket)
			m_GameThread.Cancel(*ticket);
	}

	void CancelConnectionPending(const std::uint64_t connectionId)
	{
		std::vector<std::shared_ptr<PendingRequest>> requests;
		{
			std::lock_guard<std::mutex> lock(m_JobsMutex);
			for (const auto& [key, pending] : m_Pending)
			{
				if (key.ConnectionId == connectionId)
					requests.push_back(pending);
			}
		}
		for (const auto& pending : requests)
			CancelPending(*pending);
	}

	void CancelAllPending()
	{
		std::vector<std::shared_ptr<PendingRequest>> requests;
		{
			std::lock_guard<std::mutex> lock(m_JobsMutex);
			requests.reserve(m_Pending.size());
			for (const auto& [key, pending] : m_Pending)
			{
				(void)key;
				requests.push_back(pending);
			}
		}
		for (const auto& pending : requests)
			CancelPending(*pending);
	}

	bool CloseConnection(Connection& connection)
	{
		connection.Active.store(false, std::memory_order_release);
		bool acknowledgementDrained = true;
		std::lock_guard<std::mutex> lock(connection.WriteMutex);
		if (connection.Pipe != INVALID_HANDLE_VALUE)
		{
			if (connection.GracefulClose.load(std::memory_order_acquire))
			{
				DWORD drainError = ERROR_OPERATION_ABORTED;
				if (!m_StopRequested.load(std::memory_order_acquire))
				{
					acknowledgementDrained = FlushFileBuffers(connection.Pipe) != FALSE;
					drainError = acknowledgementDrained ? ERROR_SUCCESS : GetLastError();
				}
				else
				{
					acknowledgementDrained = false;
				}
				if (!acknowledgementDrained)
				{
					RecordError(
						"PIPE_SHUTDOWN_ACK_DRAIN_FAILED",
						"Shutdown acknowledgement was not drained before disconnect",
						drainError);
				}
			}
			CancelIoEx(connection.Pipe, nullptr);
			DisconnectNamedPipe(connection.Pipe);
			CloseHandle(connection.Pipe);
			connection.Pipe = INVALID_HANDLE_VALUE;
		}
		return acknowledgementDrained;
	}

	void RequestStop()
	{
		m_StopRequested.store(true, std::memory_order_release);
		m_AdmissionsOpen.store(false, std::memory_order_release);
		if (m_StopEvent)
			SetEvent(m_StopEvent);
		if (m_Listener.joinable()
			&& GetThreadId(m_Listener.native_handle()) != GetCurrentThreadId())
		{
			CancelSynchronousIo(m_Listener.native_handle());
		}
		m_AdmissionsCondition.notify_all();
		m_JobsCondition.notify_all();
	}

	bool WaitForOwnedThreads(const std::chrono::milliseconds timeout)
	{
		if (timeout.count() < 0)
			return false;
		std::unique_lock<std::mutex> lock(m_LifecycleMutex);
		return m_LifecycleCondition.wait_for(lock, timeout, [this] {
			return m_ListenerExited.load(std::memory_order_acquire)
				&& m_WorkersExited.load(std::memory_order_acquire) == m_Workers.size();
		});
	}

	void JoinOwnedThreads()
	{
		if (m_Listener.joinable())
			m_Listener.join();
		for (std::thread& worker : m_Workers)
		{
			if (worker.joinable())
				worker.join();
		}
		m_Workers.clear();
	}

	void StopAndJoinWithoutTimeout()
	{
		RequestStop();
		CancelAllPending();
		{
			std::unique_lock<std::mutex> lock(m_LifecycleMutex);
			m_LifecycleCondition.wait(lock, [this] {
				return m_ListenerExited.load(std::memory_order_acquire)
					&& m_WorkersExited.load(std::memory_order_acquire) == m_Workers.size();
			});
		}
		JoinOwnedThreads();
		m_Started.store(false, std::memory_order_release);
		m_Listening.store(false, std::memory_order_release);
	}

	void ReleaseNativeResources()
	{
		if (m_BoundPipe != INVALID_HANDLE_VALUE)
		{
			CloseHandle(m_BoundPipe);
			m_BoundPipe = INVALID_HANDLE_VALUE;
		}
		if (m_StopEvent)
		{
			CloseHandle(m_StopEvent);
			m_StopEvent = nullptr;
		}
		if (m_SecurityDescriptor)
		{
			LocalFree(m_SecurityDescriptor);
			m_SecurityDescriptor = nullptr;
		}
	}

	Runtime::CoreRuntime& m_Runtime;
	Services::CoreCommandService& m_CommandService;
	Runtime::GameThreadExecutor& m_GameThread;
	ShutdownCallback m_ShutdownCallback;
	const std::uint32_t m_TargetProcessId;
	const std::wstring m_PipeName;

	mutable std::mutex m_StartStopMutex;
	bool m_EverStarted = false;
	std::atomic<bool> m_Started{false};
	std::atomic<bool> m_Listening{false};
	std::atomic<bool> m_AdmissionsOpen{false};
	std::atomic<bool> m_StopRequested{false};
	std::atomic<bool> m_ShutdownCallbackInvoked{false};
	HANDLE m_StopEvent = nullptr;
	HANDLE m_BoundPipe = INVALID_HANDLE_VALUE;
	std::mutex m_BoundPipeMutex;

	PSECURITY_DESCRIPTOR m_SecurityDescriptor = nullptr;
	SECURITY_ATTRIBUTES m_SecurityAttributes{};
	std::vector<std::uint8_t> m_CurrentUserSid;

	std::thread m_Listener;
	std::vector<std::thread> m_Workers;
	std::atomic<bool> m_ListenerExited{false};
	std::atomic<std::size_t> m_WorkersExited{0};
	std::atomic<std::size_t> m_LiveWorkers{0};
	std::mutex m_LifecycleMutex;
	std::condition_variable m_LifecycleCondition;

	std::mutex m_AdmissionsMutex;
	std::condition_variable m_AdmissionsCondition;
	std::mutex m_CurrentConnectionMutex;
	std::shared_ptr<Connection> m_CurrentConnection;
	std::atomic<std::uint64_t> m_NextConnectionId{1};

	std::mutex m_JobsMutex;
	std::condition_variable m_JobsCondition;
	std::deque<std::shared_ptr<PendingRequest>> m_Jobs;
	std::unordered_map<RequestKey, std::shared_ptr<PendingRequest>, RequestKeyHash> m_Pending;

	std::atomic<std::uint32_t> m_ConnectedClientProcessId{0};
	std::atomic<std::size_t> m_PendingCount{0};
	std::atomic<std::size_t> m_QueuedCount{0};
	std::atomic<std::uint64_t> m_AcceptedConnections{0};
	std::atomic<std::uint64_t> m_RejectedConnections{0};
	std::atomic<std::uint64_t> m_CompletedRequests{0};
	std::atomic<std::uint64_t> m_CancelledRequests{0};
	mutable std::mutex m_ErrorMutex;
	DWORD m_LastNativeError = ERROR_SUCCESS;
	std::string m_LastErrorCode;
	std::string m_LastErrorMessage;
};

NamedPipeRpcServer::NamedPipeRpcServer(
	Runtime::CoreRuntime& runtime,
	Services::CoreCommandService& commandService,
	Runtime::GameThreadExecutor& gameThread,
	ShutdownCallback shutdownCallback)
	: m_Impl(std::make_unique<Impl>(
		runtime,
		commandService,
		gameThread,
		std::move(shutdownCallback)))
{
}

NamedPipeRpcServer::~NamedPipeRpcServer() = default;

bool NamedPipeRpcServer::Start()
{
	return m_Impl->Start();
}

bool NamedPipeRpcServer::OpenAdmissions()
{
	return m_Impl->OpenAdmissions();
}

bool NamedPipeRpcServer::Stop(const std::chrono::milliseconds timeout)
{
	return m_Impl->Stop(timeout);
}

bool NamedPipeRpcServer::IsListening() const noexcept
{
	return m_Impl->IsListening();
}

std::wstring NamedPipeRpcServer::PipeName() const
{
	return m_Impl->PipeName();
}

NamedPipeServerDiagnostics NamedPipeRpcServer::Diagnostics() const
{
	return m_Impl->Diagnostics();
}

} // namespace UExplorer::IPC
