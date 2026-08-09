#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <utility>

namespace UExplorer::Runtime
{

class CallbackBarrier final
{
public:
	class Lease final
	{
	public:
		Lease() = default;
		Lease(const Lease&) = delete;
		Lease& operator=(const Lease&) = delete;

		Lease(Lease&& other) noexcept
			: m_Owner(std::exchange(other.m_Owner, nullptr)),
		  m_OwnedWorkAllowed(other.m_OwnedWorkAllowed)
		{
		}

		Lease& operator=(Lease&& other) noexcept
		{
			if (this == &other)
				return *this;
			Release();
			m_Owner = std::exchange(other.m_Owner, nullptr);
			m_OwnedWorkAllowed = other.m_OwnedWorkAllowed;
			return *this;
		}

		~Lease()
		{
			Release();
		}

		bool OwnedWorkAllowed() const noexcept { return m_OwnedWorkAllowed; }
		explicit operator bool() const noexcept { return m_Owner != nullptr; }

	private:
		friend class CallbackBarrier;

		Lease(CallbackBarrier* owner, const bool ownedWorkAllowed) noexcept
			: m_Owner(owner), m_OwnedWorkAllowed(ownedWorkAllowed)
		{
		}

		void Release() noexcept
		{
			if (CallbackBarrier* owner = std::exchange(m_Owner, nullptr))
				owner->Release();
		}

		CallbackBarrier* m_Owner = nullptr;
		bool m_OwnedWorkAllowed = false;
	};

	Lease Enter() noexcept
	{
		m_InFlight.fetch_add(1, std::memory_order_acq_rel);
		const bool allowed = !m_Stopping.load(std::memory_order_acquire);
		m_ActivitySequence.fetch_add(1, std::memory_order_acq_rel);
		m_Condition.notify_all();
		return Lease(this, allowed);
	}

	void BeginStopping() noexcept
	{
		m_Stopping.store(true, std::memory_order_release);
		m_ActivitySequence.fetch_add(1, std::memory_order_acq_rel);
		m_Condition.notify_all();
	}

	bool Reset() noexcept
	{
		if (m_InFlight.load(std::memory_order_acquire) != 0)
			return false;
		m_Stopping.store(false, std::memory_order_release);
		m_ActivitySequence.fetch_add(1, std::memory_order_acq_rel);
		m_Condition.notify_all();
		return true;
	}

	bool WaitForDrain(
		const std::chrono::milliseconds timeout,
		const std::chrono::milliseconds quietPeriod = std::chrono::milliseconds(10))
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		std::unique_lock<std::mutex> lock(m_Mutex);
		for (;;)
		{
			if (!m_Condition.wait_until(lock, deadline, [this] {
				return m_InFlight.load(std::memory_order_acquire) == 0;
			}))
			{
				return false;
			}

			const std::uint64_t observedSequence =
				m_ActivitySequence.load(std::memory_order_acquire);
			const auto quietDeadline = (std::min)(deadline, std::chrono::steady_clock::now() + quietPeriod);
			const bool activityObserved = m_Condition.wait_until(lock, quietDeadline, [this, observedSequence] {
				return m_InFlight.load(std::memory_order_acquire) != 0
					|| m_ActivitySequence.load(std::memory_order_acquire) != observedSequence;
			});
			if (!activityObserved
				&& m_InFlight.load(std::memory_order_acquire) == 0
				&& m_ActivitySequence.load(std::memory_order_acquire) == observedSequence)
			{
				return true;
			}
			if (std::chrono::steady_clock::now() >= deadline)
				return false;
		}
	}

	bool IsStopping() const noexcept { return m_Stopping.load(std::memory_order_acquire); }
	std::uint32_t InFlight() const noexcept { return m_InFlight.load(std::memory_order_acquire); }

private:
	void Release() noexcept
	{
		m_InFlight.fetch_sub(1, std::memory_order_acq_rel);
		m_ActivitySequence.fetch_add(1, std::memory_order_acq_rel);
		m_Condition.notify_all();
	}

	std::atomic<bool> m_Stopping{false};
	std::atomic<std::uint32_t> m_InFlight{0};
	std::atomic<std::uint64_t> m_ActivitySequence{0};
	std::mutex m_Mutex;
	std::condition_variable m_Condition;
};

} // namespace UExplorer::Runtime
