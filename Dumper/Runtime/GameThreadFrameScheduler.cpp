#include "GameThreadFrameScheduler.h"

#include <algorithm>

namespace UExplorer::Runtime
{
namespace
{

std::uint64_t ElapsedMicroseconds(
	const std::chrono::steady_clock::time_point started) noexcept
{
	const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - started).count();
	return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
}

} // namespace

bool GameThreadFrameScheduler::AttachClient(IGameThreadFrameClient& client) noexcept
{
	std::lock_guard<std::mutex> lock(m_ClientMutex);
	ClientSlot* available = nullptr;
	for (ClientSlot& slot : m_Slots)
	{
		IGameThreadFrameClient* current = slot.Client.load(std::memory_order_acquire);
		if (current == &client || slot.DrainingClient == &client)
			return false;
		if (!available && !current && !slot.DrainingClient)
			available = &slot;
	}
	if (!available || !available->Barrier.Reset())
		return false;
	available->Client.store(&client, std::memory_order_release);
	return true;
}

bool GameThreadFrameScheduler::DetachClient(
	IGameThreadFrameClient& client,
	const std::chrono::milliseconds timeout)
{
	if (timeout.count() < 0)
		return false;

	ClientSlot* target = nullptr;
	{
		std::lock_guard<std::mutex> lock(m_ClientMutex);
		for (ClientSlot& slot : m_Slots)
		{
			IGameThreadFrameClient* current = slot.Client.load(std::memory_order_acquire);
			if (current == &client)
			{
				slot.Client.store(nullptr, std::memory_order_release);
				slot.Barrier.BeginStopping();
				slot.DrainingClient = &client;
				target = &slot;
				break;
			}
			if (slot.DrainingClient == &client)
			{
				target = &slot;
				break;
			}
		}
	}

	if (!target)
		return true;
	if (!target->Barrier.WaitForDrain(timeout))
		return false;

	std::lock_guard<std::mutex> lock(m_ClientMutex);
	if (target->DrainingClient == &client)
		target->DrainingClient = nullptr;
	return true;
}

bool GameThreadFrameScheduler::HasClient(const IGameThreadFrameClient& client) const noexcept
{
	for (const ClientSlot& slot : m_Slots)
	{
		if (slot.Client.load(std::memory_order_acquire) == &client)
			return true;
	}
	return false;
}

bool GameThreadFrameScheduler::HasAnyClient() const noexcept
{
	for (const ClientSlot& slot : m_Slots)
	{
		if (slot.Client.load(std::memory_order_acquire))
			return true;
	}
	return false;
}

IGameThreadFrameClient::PumpResult GameThreadFrameScheduler::PumpFrame(
	const std::size_t workBudget) noexcept
{
	if (workBudget == 0 || workBudget > kMaxFrameWorkBudget)
	{
		m_InvalidBudgetCount.fetch_add(1, std::memory_order_acq_rel);
		return {.MoreWorkPending = HasAnyClient()};
	}
	if (m_PumpOwned.test_and_set(std::memory_order_acquire))
	{
		m_ConcurrentPumpRejectedCount.fetch_add(1, std::memory_order_acq_rel);
		return {.MoreWorkPending = HasAnyClient()};
	}
	struct PumpOwnerGuard
	{
		std::atomic_flag& Owned;
		std::atomic<bool>& Pumping;
		~PumpOwnerGuard()
		{
			Pumping.store(false, std::memory_order_release);
			Owned.clear(std::memory_order_release);
		}
	} pumpOwner{m_PumpOwned, m_Pumping};
	m_Pumping.store(true, std::memory_order_release);

	const auto started = std::chrono::steady_clock::now();
	const std::size_t startSlot = m_NextSlot.load(std::memory_order_acquire) % kMaxClients;
	std::array<std::size_t, kMaxClients> activeSlots{};
	std::array<bool, kMaxClients> eligible{};
	std::array<bool, kMaxClients> clientPending{};
	std::size_t activeCount = 0;
	for (std::size_t offset = 0; offset < kMaxClients; ++offset)
	{
		const std::size_t slotIndex = (startSlot + offset) % kMaxClients;
		if (m_Slots[slotIndex].Client.load(std::memory_order_acquire))
		{
			activeSlots[activeCount] = slotIndex;
			eligible[activeCount] = true;
			clientPending[activeCount] = true;
			++activeCount;
		}
	}

	std::size_t remaining = workBudget;
	std::size_t consumedTotal = 0;
	std::size_t dispatches = 0;
	std::size_t lastDispatchedSlot = startSlot;
	bool timeBudgetExhausted = false;
	while (remaining > 0)
	{
		bool roundProgress = false;
		bool roundEligible = false;
		for (std::size_t activeIndex = 0; activeIndex < activeCount; ++activeIndex)
		{
			if (!eligible[activeIndex])
				continue;
			roundEligible = true;
			if (ElapsedMicroseconds(started) >= kFrameTimeBudgetUs)
			{
				timeBudgetExhausted = true;
				break;
			}

			const std::size_t slotIndex = activeSlots[activeIndex];
			ClientSlot& slot = m_Slots[slotIndex];
			IGameThreadFrameClient* client = slot.Client.load(std::memory_order_acquire);
			if (!client)
			{
				eligible[activeIndex] = false;
				continue;
			}

			auto lease = slot.Barrier.Enter();
			if (!lease.OwnedWorkAllowed()
				|| slot.Client.load(std::memory_order_acquire) != client)
			{
				eligible[activeIndex] = false;
				continue;
			}

			const std::size_t allocation = (std::min)(kClientQuantum, remaining);
			const IGameThreadFrameClient::PumpResult result = client->PumpFrame(allocation);
			++dispatches;
			lastDispatchedSlot = slotIndex;

			std::size_t consumed = result.WorkConsumed;
			if (consumed > allocation)
			{
				m_ContractViolationCount.fetch_add(1, std::memory_order_acq_rel);
				consumed = allocation;
				eligible[activeIndex] = false;
			}
			else
			{
				eligible[activeIndex] = result.MoreWorkPending;
			}

			clientPending[activeIndex] = result.MoreWorkPending;
			if (consumed == 0)
			{
				if (result.MoreWorkPending)
					m_ZeroProgressCount.fetch_add(1, std::memory_order_acq_rel);
				eligible[activeIndex] = false;
			}
			else
			{
				roundProgress = true;
				remaining -= consumed;
				consumedTotal += consumed;
			}

			if (remaining == 0)
				break;
		}

		if (timeBudgetExhausted || !roundEligible || !roundProgress)
			break;
	}

	if (timeBudgetExhausted)
		m_TimeBudgetExhaustions.fetch_add(1, std::memory_order_acq_rel);
	if (dispatches > 0)
		m_NextSlot.store((lastDispatchedSlot + 1) % kMaxClients, std::memory_order_release);
	m_LastFrameBudget.store(workBudget, std::memory_order_release);
	m_LastFrameWorkConsumed.store(consumedTotal, std::memory_order_release);
	m_LastFrameDispatches.store(dispatches, std::memory_order_release);
	m_LastFrameDurationUs.store(ElapsedMicroseconds(started), std::memory_order_release);
	m_FrameCount.fetch_add(1, std::memory_order_acq_rel);
	bool pending = false;
	for (std::size_t activeIndex = 0; activeIndex < activeCount; ++activeIndex)
		pending = pending || clientPending[activeIndex];
	return {
		.WorkConsumed = consumedTotal,
		.MoreWorkPending = pending
	};
}

GameThreadFrameSchedulerDiagnostics GameThreadFrameScheduler::Diagnostics() const noexcept
{
	GameThreadFrameSchedulerDiagnostics diagnostics{
		.Pumping = m_Pumping.load(std::memory_order_acquire),
		.FrameCount = m_FrameCount.load(std::memory_order_acquire),
		.LastFrameDurationUs = m_LastFrameDurationUs.load(std::memory_order_acquire),
		.LastFrameBudget = m_LastFrameBudget.load(std::memory_order_acquire),
		.LastFrameWorkConsumed = m_LastFrameWorkConsumed.load(std::memory_order_acquire),
		.LastFrameDispatches = m_LastFrameDispatches.load(std::memory_order_acquire),
		.TimeBudgetExhaustions = m_TimeBudgetExhaustions.load(std::memory_order_acquire),
		.InvalidBudgetCount = m_InvalidBudgetCount.load(std::memory_order_acquire),
		.ContractViolationCount = m_ContractViolationCount.load(std::memory_order_acquire),
		.ZeroProgressCount = m_ZeroProgressCount.load(std::memory_order_acquire),
		.ConcurrentPumpRejectedCount =
			m_ConcurrentPumpRejectedCount.load(std::memory_order_acquire)
	};

	std::lock_guard<std::mutex> lock(m_ClientMutex);
	for (const ClientSlot& slot : m_Slots)
	{
		if (slot.Client.load(std::memory_order_acquire))
			++diagnostics.AttachedClients;
		if (slot.DrainingClient)
			++diagnostics.DrainingClients;
		diagnostics.InFlightCallbacks += slot.Barrier.InFlight();
	}
	return diagnostics;
}

GameThreadFrameScheduler& GetGameThreadFrameScheduler()
{
	static GameThreadFrameScheduler scheduler;
	return scheduler;
}

} // namespace UExplorer::Runtime
