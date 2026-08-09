#pragma once

#include "ObjectHandle.h"

#include <cstdint>

namespace UExplorer::Runtime
{

enum class ObjectSnapshotSlotReadResult : std::uint8_t
{
	Captured,
	Empty,
	Failed
};

// Extends executable identity reads with an explicit empty-slot state for snapshots.
class IObjectSnapshotIdentitySource : public IHandleIdentitySource
{
public:
	virtual bool CanReadObjectSlots() const noexcept = 0;
	virtual bool TryGetObjectCount(std::int32_t& objectCount) = 0;
	virtual ObjectSnapshotSlotReadResult TryReadObjectSlot(
		std::int32_t index,
		ObjectIdentity& identity) = 0;
};

} // namespace UExplorer::Runtime
