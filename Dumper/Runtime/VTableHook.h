#pragma once

#include <cstdint>
#include <memory>

namespace UExplorer::Runtime
{

enum class VTableHookError : std::uint8_t
{
	None,
	InvalidArgument,
	ReadFailed,
	OriginalMismatch,
	AlreadyInstalled,
	AllocationFailed,
	PatchFailed,
	RestoreFailed
};

const char* ToString(VTableHookError error) noexcept;

class VTableHookToken;

struct VTableHookInstallResult
{
	VTableHookError Error = VTableHookError::None;
	std::unique_ptr<VTableHookToken> Token;

	bool Ok() const noexcept { return Error == VTableHookError::None && Token != nullptr; }
};

class VTableHookToken final
{
public:
	static VTableHookInstallResult Install(
		void** slot,
		void* replacement,
		void* expectedOriginal = nullptr);

	VTableHookToken(const VTableHookToken&) = delete;
	VTableHookToken& operator=(const VTableHookToken&) = delete;
	VTableHookToken(VTableHookToken&&) = delete;
	VTableHookToken& operator=(VTableHookToken&&) = delete;
	~VTableHookToken();

	bool Enable() noexcept;
	bool Disable() noexcept;
	bool IsActive() const noexcept { return m_Active; }
	bool SafeToUnload() const noexcept { return !m_Active; }
	void** Slot() const noexcept { return m_Slot; }
	void* Original() const noexcept { return m_Original; }
	void* Replacement() const noexcept { return m_Replacement; }
	VTableHookError LastError() const noexcept { return m_LastError; }

private:
	VTableHookToken(void** slot, void* original, void* replacement) noexcept;
	bool ReadCurrent(void*& current) noexcept;

	void** m_Slot = nullptr;
	void* m_Original = nullptr;
	void* m_Replacement = nullptr;
	VTableHookError m_LastError = VTableHookError::None;
	bool m_Active = false;
};

} // namespace UExplorer::Runtime
