#include "PostRenderHook.h"

#include "GameThreadExecutor.h"
#include "SafeMemory.h"
#include "OffsetFinder/Offsets.h"
#include "Unreal/ObjectArray.h"
#include "Unreal/UnrealObjects.h"

#include <iostream>
#include <limits>
#include <utility>

namespace UExplorer::Runtime
{
namespace
{
	constexpr std::int32_t kMaxVTableIndex = 512;

	bool ReadObjectVTable(void* object, void**& vtable)
	{
		vtable = nullptr;
		return object
			&& ReadValue(reinterpret_cast<std::uintptr_t>(object), vtable).Ok()
			&& vtable;
	}

	bool ReadPointerSlot(void** slot, void*& value)
	{
		value = nullptr;
		return slot
			&& ReadValue(reinterpret_cast<std::uintptr_t>(slot), value).Ok();
	}
}

void PostRenderHook::HookedPostRender(void* viewportClient, void* canvas)
{
	PostRenderHook* owner = s_Active.load(std::memory_order_acquire);
	if (!owner)
		return;

	auto callback = owner->m_CallbackBarrier.Enter();
	if (callback.OwnedWorkAllowed()
		&& !owner->m_Stopping.load(std::memory_order_acquire))
	{
		GetPostRenderPumpBackend().Tick();
	}

	auto original = reinterpret_cast<PostRenderFn>(
		owner->m_Original.load(std::memory_order_acquire));
	if (original)
		original(viewportClient, canvas);
}

bool PostRenderHook::Install()
{
	if (m_Patch && m_Patch->IsActive())
		return true;
	if (m_Patch || m_Stopping.load(std::memory_order_acquire))
		return false;
	if (!m_CallbackBarrier.Reset())
		return false;

	UEClass viewportClass = ObjectArray::FindClassFast("GameViewportClient");
	if (!viewportClass)
	{
		std::cerr << "[PostRenderHook] GameViewportClient class not found\n";
		return false;
	}
	UEObject viewportCdo = viewportClass.GetDefaultObject();
	void** viewportVTable = nullptr;
	if (!viewportCdo || !ReadObjectVTable(viewportCdo.GetAddress(), viewportVTable))
	{
		std::cerr << "[PostRenderHook] GameViewportClient CDO vtable is unavailable\n";
		return false;
	}

	const std::int32_t postRenderIndex = Off::InSDK::PostRender::GVCPostRenderIndex;
	if (postRenderIndex < 0 || postRenderIndex >= kMaxVTableIndex)
	{
		std::cerr << "[PostRenderHook] PostRender index is invalid\n";
		return false;
	}
	void** postRenderSlot = viewportVTable + postRenderIndex;
	void* originalPostRender = nullptr;
	if (!ReadPointerSlot(postRenderSlot, originalPostRender) || !originalPostRender)
	{
		std::cerr << "[PostRenderHook] Original PostRender pointer is unavailable\n";
		return false;
	}

	UEClass objectClass = ObjectArray::FindClassFast("Object");
	UEObject objectCdo = objectClass ? objectClass.GetDefaultObject() : UEObject{};
	void** objectVTable = nullptr;
	const std::int32_t processEventIndex = Off::InSDK::ProcessEvent::PEIndex;
	void* processEvent = nullptr;
	if (!objectCdo
		|| processEventIndex <= 0
		|| processEventIndex >= kMaxVTableIndex
		|| !ReadObjectVTable(objectCdo.GetAddress(), objectVTable)
		|| !ReadPointerSlot(objectVTable + processEventIndex, processEvent)
		|| !processEvent)
	{
		std::cerr << "[PostRenderHook] Validated ProcessEvent pointer is unavailable\n";
		return false;
	}

	m_Original.store(originalPostRender, std::memory_order_release);
	PostRenderHook* expected = nullptr;
	if (!s_Active.compare_exchange_strong(
		expected,
		this,
		std::memory_order_acq_rel,
		std::memory_order_acquire))
	{
		m_Original.store(nullptr, std::memory_order_release);
		return false;
	}

	VTableHookInstallResult installed = VTableHookToken::Install(
		postRenderSlot,
		reinterpret_cast<void*>(&HookedPostRender),
		originalPostRender);
	if (!installed.Ok())
	{
		s_Active.store(nullptr, std::memory_order_release);
		m_Original.store(nullptr, std::memory_order_release);
		return false;
	}
	m_Patch = std::move(installed.Token);
	m_Installed.store(true, std::memory_order_release);

	if (!GetGameThreadExecutor().Enable(
		reinterpret_cast<ProcessEventFn>(processEvent)))
	{
		m_Stopping.store(true, std::memory_order_release);
		if (!DisablePatch(std::chrono::milliseconds(5000), "executor enable rollback"))
			return false;
		m_Stopping.store(false, std::memory_order_release);
		return false;
	}

	std::cerr << "[PostRenderHook] installed: index=" << postRenderIndex << "\n";
	return true;
}

bool PostRenderHook::DisablePatch(
	const std::chrono::milliseconds timeout,
	const char* context)
{
	m_CallbackBarrier.BeginStopping();
	if (!m_Patch)
	{
		m_Installed.store(false, std::memory_order_release);
		m_Original.store(nullptr, std::memory_order_release);
		PostRenderHook* expected = this;
		s_Active.compare_exchange_strong(
			expected,
			nullptr,
			std::memory_order_acq_rel,
			std::memory_order_acquire);
		return expected == this || expected == nullptr;
	}
	if (m_Patch && m_Patch->IsActive() && !m_Patch->Disable())
	{
		std::cerr << "[PostRenderHook] " << context
			<< ": VTable restore failed; unload is unsafe\n";
		return false;
	}
	m_Installed.store(false, std::memory_order_release);
	if (!m_CallbackBarrier.WaitForDrain(timeout))
	{
		std::cerr << "[PostRenderHook] " << context
			<< ": callback drain timed out: in_flight="
			<< m_CallbackBarrier.InFlight() << "\n";
		return false;
	}
	m_Patch.reset();
	m_Original.store(nullptr, std::memory_order_release);
	PostRenderHook* expected = this;
	if (!s_Active.compare_exchange_strong(
		expected,
		nullptr,
		std::memory_order_acq_rel,
		std::memory_order_acquire))
	{
		return false;
	}
	return true;
}

bool PostRenderHook::Stop(const std::chrono::milliseconds timeout)
{
	if (timeout.count() <= 0
		|| timeout.count() > (std::numeric_limits<int>::max)())
	{
		return false;
	}
	m_Stopping.store(true, std::memory_order_release);
	m_CallbackBarrier.BeginStopping();
	if (!GetGameThreadExecutor().DisableAndDrain(static_cast<int>(timeout.count())))
	{
		std::cerr << "[PostRenderHook] game-thread executor drain timed out\n";
		return false;
	}
	if (!DisablePatch(timeout, "shutdown"))
		return false;
	std::cerr << "[PostRenderHook] stopped\n";
	return true;
}

PostRenderHookDiagnostics PostRenderHook::Diagnostics() const noexcept
{
	return {
		.Installed = m_Installed.load(std::memory_order_acquire),
		.Stopping = m_Stopping.load(std::memory_order_acquire),
		.InFlight = m_CallbackBarrier.InFlight()
	};
}

} // namespace UExplorer::Runtime
