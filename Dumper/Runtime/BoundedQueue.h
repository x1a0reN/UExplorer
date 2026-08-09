#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace UExplorer::Runtime
{
	enum class OverflowPolicy
	{
		RejectNewest,
		DropOldest,
	};

	enum class EnqueueResult
	{
		Accepted,
		DroppedOldest,
		Full,
		Closed,
	};

	template <typename T>
	class BoundedQueue
	{
	public:
		explicit BoundedQueue(const std::size_t capacity, const OverflowPolicy overflowPolicy)
			: m_Capacity(capacity), m_OverflowPolicy(overflowPolicy)
		{
			if (capacity == 0)
				throw std::invalid_argument("BoundedQueue capacity must be greater than zero");
		}

		BoundedQueue(const BoundedQueue&) = delete;
		BoundedQueue& operator=(const BoundedQueue&) = delete;

		EnqueueResult Enqueue(T item)
		{
			std::unique_lock lock(m_Mutex);
			if (m_Closed)
				return EnqueueResult::Closed;

			EnqueueResult result = EnqueueResult::Accepted;
			if (m_Items.size() == m_Capacity)
			{
				if (m_OverflowPolicy == OverflowPolicy::RejectNewest)
					return EnqueueResult::Full;
				m_Items.pop_front();
				++m_DroppedCount;
				result = EnqueueResult::DroppedOldest;
			}

			m_Items.push_back(std::move(item));
			lock.unlock();
			m_Ready.notify_one();
			return result;
		}

		std::optional<T> TryPop()
		{
			std::scoped_lock lock(m_Mutex);
			if (m_Items.empty())
				return std::nullopt;
			T item = std::move(m_Items.front());
			m_Items.pop_front();
			return item;
		}

		std::optional<T> WaitPop()
		{
			std::unique_lock lock(m_Mutex);
			m_Ready.wait(lock, [this] { return m_Closed || !m_Items.empty(); });
			if (m_Items.empty())
				return std::nullopt;
			T item = std::move(m_Items.front());
			m_Items.pop_front();
			return item;
		}

		void Close()
		{
			{
				std::scoped_lock lock(m_Mutex);
				m_Closed = true;
			}
			m_Ready.notify_all();
		}

		std::vector<T> CloseAndDrain()
		{
			std::vector<T> drained;
			{
				std::scoped_lock lock(m_Mutex);
				m_Closed = true;
				drained.reserve(m_Items.size());
				while (!m_Items.empty())
				{
					drained.push_back(std::move(m_Items.front()));
					m_Items.pop_front();
				}
			}
			m_Ready.notify_all();
			return drained;
		}

		std::size_t Size() const
		{
			std::scoped_lock lock(m_Mutex);
			return m_Items.size();
		}

		std::uint64_t DroppedCount() const
		{
			std::scoped_lock lock(m_Mutex);
			return m_DroppedCount;
		}

		bool IsClosed() const
		{
			std::scoped_lock lock(m_Mutex);
			return m_Closed;
		}

	private:
		const std::size_t m_Capacity;
		const OverflowPolicy m_OverflowPolicy;
		mutable std::mutex m_Mutex;
		std::condition_variable m_Ready;
		std::deque<T> m_Items;
		std::uint64_t m_DroppedCount = 0;
		bool m_Closed = false;
	};
}
