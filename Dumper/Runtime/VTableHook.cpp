#include "VTableHook.h"

#include "SafeMemory.h"

#include <new>
#include <utility>

namespace UExplorer::Runtime
{

const char* ToString(const VTableHookError error) noexcept
{
	switch (error)
	{
	case VTableHookError::None: return "NONE";
	case VTableHookError::InvalidArgument: return "VTABLE_HOOK_INVALID_ARGUMENT";
	case VTableHookError::ReadFailed: return "VTABLE_HOOK_READ_FAILED";
	case VTableHookError::OriginalMismatch: return "VTABLE_HOOK_ORIGINAL_MISMATCH";
	case VTableHookError::AlreadyInstalled: return "VTABLE_HOOK_ALREADY_INSTALLED";
	case VTableHookError::AllocationFailed: return "VTABLE_HOOK_ALLOCATION_FAILED";
	case VTableHookError::PatchFailed: return "VTABLE_HOOK_PATCH_FAILED";
	case VTableHookError::RestoreFailed: return "VTABLE_HOOK_RESTORE_FAILED";
	}
	return "VTABLE_HOOK_UNKNOWN_ERROR";
}

VTableHookToken::VTableHookToken(void** slot, void* original, void* replacement) noexcept
	: m_Slot(slot),
	  m_Original(original),
	  m_Replacement(replacement)
{
}

VTableHookToken::~VTableHookToken()
{
	if (m_Active)
		Disable();
}

bool VTableHookToken::ReadCurrent(void*& current) noexcept
{
	current = nullptr;
	if (!m_Slot
		|| !ReadValue(reinterpret_cast<std::uintptr_t>(m_Slot), current).Ok())
	{
		m_LastError = VTableHookError::ReadFailed;
		return false;
	}
	return true;
}

VTableHookInstallResult VTableHookToken::Install(
	void** slot,
	void* replacement,
	void* expectedOriginal)
{
	if (!slot || !replacement)
		return {.Error = VTableHookError::InvalidArgument};

	void* current = nullptr;
	if (!ReadValue(reinterpret_cast<std::uintptr_t>(slot), current).Ok() || !current)
		return {.Error = VTableHookError::ReadFailed};
	if (current == replacement)
		return {.Error = VTableHookError::AlreadyInstalled};
	if (expectedOriginal && current != expectedOriginal)
		return {.Error = VTableHookError::OriginalMismatch};

	// Allocate ownership before changing executable state. Allocation failure must
	// never leave a patched slot without a token that can restore it.
	std::unique_ptr<VTableHookToken> token(
		new (std::nothrow) VTableHookToken(slot, current, replacement));
	if (!token)
		return {.Error = VTableHookError::AllocationFailed};
	if (!CompareExchangePointer(slot, current, replacement).Ok())
		return {.Error = VTableHookError::PatchFailed};
	token->m_Active = true;

	return {
		.Token = std::move(token)
	};
}

bool VTableHookToken::Enable() noexcept
{
	void* current = nullptr;
	if (!ReadCurrent(current))
		return false;
	if (m_Active)
	{
		if (current == m_Replacement)
		{
			m_LastError = VTableHookError::None;
			return true;
		}
		m_Active = false;
		m_LastError = VTableHookError::OriginalMismatch;
		return false;
	}
	if (current == m_Replacement)
	{
		m_Active = true;
		m_LastError = VTableHookError::None;
		return true;
	}
	if (current != m_Original)
	{
		m_LastError = VTableHookError::OriginalMismatch;
		return false;
	}
	if (!CompareExchangePointer(m_Slot, m_Original, m_Replacement).Ok())
	{
		m_LastError = VTableHookError::PatchFailed;
		return false;
	}
	m_Active = true;
	m_LastError = VTableHookError::None;
	return true;
}

bool VTableHookToken::Disable() noexcept
{
	if (!m_Active)
		return true;
	const MemoryResult restore = CompareExchangePointer(m_Slot, m_Replacement, m_Original);
	if (restore.Ok())
	{
		m_Active = false;
		m_LastError = VTableHookError::None;
		return true;
	}
	if (restore.Error == MemoryError::ValueMismatch)
	{
		void* current = nullptr;
		if (ReadCurrent(current) && current != m_Replacement)
		{
			m_Active = false;
			m_LastError = VTableHookError::OriginalMismatch;
			return true;
		}
	}
	m_LastError = VTableHookError::RestoreFailed;
	return false;
}

} // namespace UExplorer::Runtime
