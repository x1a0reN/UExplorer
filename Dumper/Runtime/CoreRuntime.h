#pragma once

#include "CapabilityRegistry.h"
#include "EngineContext.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace UExplorer::Runtime
{

enum class CoreState : std::uint8_t
{
	Created,
	Initializing,
	Ready,
	Failed,
	Stopping,
	Stopped
};

inline const char* ToString(const CoreState state)
{
	switch (state)
	{
	case CoreState::Created:
		return "created";
	case CoreState::Initializing:
		return "initializing";
	case CoreState::Ready:
		return "ready";
	case CoreState::Failed:
		return "failed";
	case CoreState::Stopping:
		return "stopping";
	case CoreState::Stopped:
		return "stopped";
	}
	return "unknown";
}

struct CoreRuntimeSnapshot
{
	CoreState State = CoreState::Created;
	std::string SessionId;
	std::shared_ptr<const EngineContext> Context;
	std::shared_ptr<const CapabilitySnapshot> Capabilities;
	std::vector<std::string> ReadinessBlockers;
	std::string FailureCode;
	std::string FailureMessage;
	std::size_t ActiveRequests = 0;
	bool ReadinessSatisfied = false;

	bool IsLive() const noexcept { return State != CoreState::Stopped; }
	bool IsReady() const noexcept { return State == CoreState::Ready && ReadinessSatisfied; }
};

class CoreRuntime final
{
public:
	class RequestLease final
	{
	public:
		RequestLease() = default;
		RequestLease(const RequestLease&) = delete;
		RequestLease& operator=(const RequestLease&) = delete;

		RequestLease(RequestLease&& other) noexcept
			: m_Owner(std::exchange(other.m_Owner, nullptr)),
			  m_Context(std::move(other.m_Context)),
			  m_Capabilities(std::move(other.m_Capabilities))
		{
		}

		RequestLease& operator=(RequestLease&& other) noexcept
		{
			if (this == &other)
				return *this;
			Release();
			m_Owner = std::exchange(other.m_Owner, nullptr);
			m_Context = std::move(other.m_Context);
			m_Capabilities = std::move(other.m_Capabilities);
			return *this;
		}

		~RequestLease()
		{
			Release();
		}

		const std::shared_ptr<const EngineContext>& Context() const noexcept { return m_Context; }
		const std::shared_ptr<const CapabilitySnapshot>& Capabilities() const noexcept { return m_Capabilities; }
		explicit operator bool() const noexcept { return m_Owner != nullptr; }

	private:
		friend class CoreRuntime;

		RequestLease(
			CoreRuntime* owner,
			std::shared_ptr<const EngineContext> context,
			std::shared_ptr<const CapabilitySnapshot> capabilities)
			: m_Owner(owner),
			  m_Context(std::move(context)),
			  m_Capabilities(std::move(capabilities))
		{
		}

		void Release()
		{
			if (CoreRuntime* owner = std::exchange(m_Owner, nullptr))
				owner->ReleaseRequest();
		}

		CoreRuntime* m_Owner = nullptr;
		std::shared_ptr<const EngineContext> m_Context;
		std::shared_ptr<const CapabilitySnapshot> m_Capabilities;
	};

	bool BeginInitialize(std::string sessionId)
	{
		if (sessionId.empty() || sessionId.size() > 128)
			return false;
		for (const char value : sessionId)
		{
			const bool valid = (value >= '0' && value <= '9')
				|| (value >= 'A' && value <= 'Z')
				|| (value >= 'a' && value <= 'z')
				|| value == '-'
				|| value == '_';
			if (!valid)
				return false;
		}
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_State != CoreState::Created)
			return false;
		m_SessionId = std::move(sessionId);
		m_State = CoreState::Initializing;
		m_ReadinessBlockers.clear();
		return true;
	}

	bool PublishContext(std::shared_ptr<const EngineContext> context)
	{
		if (!context)
			return false;
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_State != CoreState::Initializing || m_Context)
			return false;
		m_Context = std::move(context);
		return true;
	}

	bool PublishCapabilities(std::shared_ptr<const CapabilitySnapshot> capabilities)
	{
		if (!capabilities)
			return false;
		std::lock_guard<std::mutex> lock(m_Mutex);
		if ((m_State != CoreState::Initializing && m_State != CoreState::Ready)
			|| !m_Context
			|| capabilities->ContextGeneration() != m_Context->Generation())
		{
			return false;
		}
		m_Capabilities = std::move(capabilities);
		if (m_State == CoreState::Ready && !m_RequiredCapabilities.empty())
			EvaluateReadinessLocked();
		return true;
	}

	bool TryMarkReady(
		const std::vector<std::string>& requiredCapabilities,
		std::vector<std::string>* blockers = nullptr)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_State != CoreState::Initializing || !m_Context || !m_Capabilities)
			return false;

		m_RequiredCapabilities = requiredCapabilities;
		EvaluateReadinessLocked();

		if (blockers)
			*blockers = m_ReadinessBlockers;
		if (!m_ReadinessSatisfied)
			return false;

		m_State = CoreState::Ready;
		return true;
	}

	bool MarkFailed(std::string code, std::string message)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_State != CoreState::Created && m_State != CoreState::Initializing)
			return false;
		m_State = CoreState::Failed;
		m_ReadinessSatisfied = false;
		m_FailureCode = std::move(code);
		m_FailureMessage = std::move(message);
		m_Condition.notify_all();
		return true;
	}

	bool BeginStopping()
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_State == CoreState::Stopping || m_State == CoreState::Stopped)
			return false;
		m_State = CoreState::Stopping;
		m_ReadinessSatisfied = false;
		m_Condition.notify_all();
		return true;
	}

	std::optional<RequestLease> TryAcquireRequest(std::string* errorCode = nullptr)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_State != CoreState::Ready || !m_ReadinessSatisfied || !m_Context || !m_Capabilities)
		{
			if (errorCode)
				*errorCode = m_State == CoreState::Stopping || m_State == CoreState::Stopped
					? "CORE_STOPPING"
					: "CORE_NOT_READY";
			return std::nullopt;
		}
		++m_ActiveRequests;
		return RequestLease(this, m_Context, m_Capabilities);
	}

	bool WaitForRequests(const std::chrono::milliseconds timeout)
	{
		std::unique_lock<std::mutex> lock(m_Mutex);
		return m_Condition.wait_for(lock, timeout, [this] { return m_ActiveRequests == 0; });
	}

	bool MarkStopped()
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_State != CoreState::Stopping || m_ActiveRequests != 0)
			return false;
		m_State = CoreState::Stopped;
		m_ReadinessSatisfied = false;
		m_Condition.notify_all();
		return true;
	}

	void RecordShutdownFailure(std::string code, std::string message)
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_State != CoreState::Stopping)
			return;
		m_FailureCode = std::move(code);
		m_FailureMessage = std::move(message);
	}

	CoreRuntimeSnapshot Snapshot() const
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		return {
			.State = m_State,
			.SessionId = m_SessionId,
			.Context = m_Context,
			.Capabilities = m_Capabilities,
			.ReadinessBlockers = m_ReadinessBlockers,
			.FailureCode = m_FailureCode,
			.FailureMessage = m_FailureMessage,
			.ActiveRequests = m_ActiveRequests,
			.ReadinessSatisfied = m_ReadinessSatisfied
		};
	}

private:
	void EvaluateReadinessLocked()
	{
		m_ReadinessBlockers.clear();
		for (const std::string& name : m_RequiredCapabilities)
		{
			const CapabilityStatus* capability = m_Capabilities->Find(name);
			if (!capability || !capability->Available)
			{
				std::string blocker = name;
				if (capability && !capability->ReasonCode.empty())
					blocker += ":" + capability->ReasonCode;
				else
					blocker += ":CAPABILITY_MISSING";
				m_ReadinessBlockers.push_back(std::move(blocker));
			}
		}
		m_ReadinessSatisfied = m_ReadinessBlockers.empty();
	}

	void ReleaseRequest()
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		if (m_ActiveRequests > 0)
			--m_ActiveRequests;
		m_Condition.notify_all();
	}

	mutable std::mutex m_Mutex;
	std::condition_variable m_Condition;
	CoreState m_State = CoreState::Created;
	std::string m_SessionId;
	std::shared_ptr<const EngineContext> m_Context;
	std::shared_ptr<const CapabilitySnapshot> m_Capabilities;
	std::vector<std::string> m_RequiredCapabilities;
	std::vector<std::string> m_ReadinessBlockers;
	std::string m_FailureCode;
	std::string m_FailureMessage;
	std::size_t m_ActiveRequests = 0;
	bool m_ReadinessSatisfied = false;
};

} // namespace UExplorer::Runtime
