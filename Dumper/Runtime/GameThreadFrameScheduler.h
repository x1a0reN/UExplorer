#pragma once

#include "CallbackBarrier.h"
#include "GameThreadExecutor.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace UExplorer::Runtime
{

struct GameThreadFrameSchedulerDiagnostics
{
	bool Pumping = false;
	std::size_t AttachedClients = 0;
	std::size_t DrainingClients = 0;
	std::size_t InFlightCallbacks = 0;
	std::uint64_t FrameCount = 0;
	std::uint64_t LastFrameDurationUs = 0;
	std::size_t LastFrameBudget = 0;
	std::size_t LastFrameWorkConsumed = 0;
	std::size_t LastFrameDispatches = 0;
	std::uint64_t TimeBudgetExhaustions = 0;
	std::uint64_t InvalidBudgetCount = 0;
	std::uint64_t ContractViolationCount = 0;
	std::uint64_t ZeroProgressCount = 0;
	std::uint64_t ConcurrentPumpRejectedCount = 0;
};

class GameThreadFrameScheduler final : public IGameThreadFrameClient
{
public:
	static constexpr std::size_t kMaxClients = 8;
	static constexpr std::size_t kMaxFrameWorkBudget =
		PostRenderPumpBackend::kFrameWorkBudget;
	static constexpr std::size_t kClientQuantum = 4;
	static constexpr std::uint64_t kFrameTimeBudgetUs = 2'000;

	GameThreadFrameScheduler() = default;
	GameThreadFrameScheduler(const GameThreadFrameScheduler&) = delete;
	GameThreadFrameScheduler& operator=(const GameThreadFrameScheduler&) = delete;

	bool AttachClient(IGameThreadFrameClient& client) noexcept;
	bool DetachClient(
		IGameThreadFrameClient& client,
		std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));
	bool HasClient(const IGameThreadFrameClient& client) const noexcept;
	IGameThreadFrameClient::PumpResult PumpFrame(std::size_t workBudget) noexcept override;
	GameThreadFrameSchedulerDiagnostics Diagnostics() const noexcept;

private:
	struct ClientSlot
	{
		std::atomic<IGameThreadFrameClient*> Client{nullptr};
		CallbackBarrier Barrier;
		IGameThreadFrameClient* DrainingClient = nullptr;
	};

	bool HasAnyClient() const noexcept;

	std::array<ClientSlot, kMaxClients> m_Slots;
	mutable std::mutex m_ClientMutex;
	std::atomic_flag m_PumpOwned = ATOMIC_FLAG_INIT;
	std::atomic<bool> m_Pumping{false};
	std::atomic<std::size_t> m_NextSlot{0};
	std::atomic<std::uint64_t> m_FrameCount{0};
	std::atomic<std::uint64_t> m_LastFrameDurationUs{0};
	std::atomic<std::size_t> m_LastFrameBudget{0};
	std::atomic<std::size_t> m_LastFrameWorkConsumed{0};
	std::atomic<std::size_t> m_LastFrameDispatches{0};
	std::atomic<std::uint64_t> m_TimeBudgetExhaustions{0};
	std::atomic<std::uint64_t> m_InvalidBudgetCount{0};
	std::atomic<std::uint64_t> m_ContractViolationCount{0};
	std::atomic<std::uint64_t> m_ZeroProgressCount{0};
	std::atomic<std::uint64_t> m_ConcurrentPumpRejectedCount{0};
};

GameThreadFrameScheduler& GetGameThreadFrameScheduler();

} // namespace UExplorer::Runtime
