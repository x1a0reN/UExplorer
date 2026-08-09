#pragma once

#include "CallbackBarrier.h"
#include "VTableHook.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace UExplorer::Runtime
{

struct PostRenderHookDiagnostics
{
	bool Installed = false;
	bool Stopping = false;
	std::uint64_t InFlight = 0;
};

class PostRenderHook final
{
public:
	PostRenderHook() = default;
	~PostRenderHook() = default;

	PostRenderHook(const PostRenderHook&) = delete;
	PostRenderHook& operator=(const PostRenderHook&) = delete;

	bool Install();
	bool Stop(std::chrono::milliseconds timeout);
	PostRenderHookDiagnostics Diagnostics() const noexcept;

private:
	using PostRenderFn = void(*)(void*, void*);

	static void HookedPostRender(void* viewportClient, void* canvas);
	bool DisablePatch(std::chrono::milliseconds timeout, const char* context);

	inline static std::atomic<PostRenderHook*> s_Active{nullptr};
	std::unique_ptr<VTableHookToken> m_Patch;
	std::atomic<void*> m_Original{nullptr};
	std::atomic<bool> m_Installed{false};
	std::atomic<bool> m_Stopping{false};
	CallbackBarrier m_CallbackBarrier;
};

} // namespace UExplorer::Runtime
