#pragma once

#include "EngineContext.h"
#include "ObjectIdentityContext.h"
#include "ObjectSnapshotIdentitySource.h"

#include <memory>

namespace UExplorer::Runtime
{

class ObjectArrayIdentitySource final : public IObjectSnapshotIdentitySource
{
public:
	explicit ObjectArrayIdentitySource(std::shared_ptr<const EngineContext> context);

	std::uint64_t ContextGeneration() const noexcept override { return m_Offsets.ContextGeneration; }
	bool CanIssueObjectHandles() const noexcept;
	bool CanIssueFunctionHandles() const noexcept;
	bool CanReadObjectSlots() const noexcept override { return CanIssueObjectHandles(); }
	bool IsCurrentExecutionThreadValid() const noexcept override;
	bool TryGetObjectCount(std::int32_t& objectCount) override;
	ObjectSnapshotSlotReadResult TryReadObjectSlot(
		std::int32_t index,
		ObjectIdentity& identity) override;
	bool TryReadObject(std::int32_t index, ObjectIdentity& identity) override;
	bool TryReadFunction(std::int32_t index, FunctionIdentity& identity) override;

private:
	bool TryReadObjectCore(std::int32_t index, ObjectIdentity& identity) const;

	std::shared_ptr<const EngineContext> m_Context;
	ObjectIdentityContext m_Offsets;
};

} // namespace UExplorer::Runtime
