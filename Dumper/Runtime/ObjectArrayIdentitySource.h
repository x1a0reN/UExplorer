#pragma once

#include "ObjectHandle.h"

namespace UExplorer::Runtime
{

class ObjectArrayIdentitySource final : public IHandleIdentitySource
{
public:
	bool IsLayoutAvailable() const noexcept;
	bool IsCurrentExecutionThreadValid() const noexcept;
	bool TryReadObject(std::int32_t index, ObjectIdentity& identity) override;
	bool TryReadFunction(std::int32_t index, FunctionIdentity& identity) override;

private:
	bool TryReadObjectCore(std::int32_t index, ObjectIdentity& identity) const;
};

ObjectArrayIdentitySource& GetObjectArrayIdentitySource();

} // namespace UExplorer::Runtime
