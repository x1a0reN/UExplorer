#pragma once

#include "EngineContext.h"
#include "EngineSnapshot.h"
#include "ObjectHandle.h"

#include <memory>
#include <string>

namespace UExplorer::Runtime
{

// Owns the immutable engine generation boundary used by every live domain command.
class EngineFacade final
{
public:
	EngineFacade(
		std::shared_ptr<const EngineContext> context,
		std::string sessionId,
		IHandleIdentitySource& identitySource);
	EngineFacade(const EngineFacade&) = delete;
	EngineFacade& operator=(const EngineFacade&) = delete;

	bool IsConfigured() const noexcept;
	const std::string& SessionId() const noexcept { return m_SessionId; }
	std::uint64_t ContextGeneration() const noexcept;
	const std::shared_ptr<const EngineContext>& Context() const noexcept { return m_Context; }
	bool IsCurrentExecutionThreadValid() const noexcept;

	ObjectHandleResult IssueObjectHandle(std::int32_t index);
	ObjectValidationResult ValidateObjectHandle(const ObjectHandle& handle);
	FunctionHandleResult IssueFunctionHandle(std::int32_t index);
	FunctionValidationResult ValidateFunctionHandle(const FunctionHandle& handle);

	EngineSnapshotStore& Snapshots() noexcept { return m_Snapshots; }
	const EngineSnapshotStore& Snapshots() const noexcept { return m_Snapshots; }
	void Stop() noexcept;

private:
	std::shared_ptr<const EngineContext> m_Context;
	std::string m_SessionId;
	IHandleIdentitySource& m_IdentitySource;
	ObjectHandleService m_Handles;
	EngineSnapshotStore m_Snapshots;
};

} // namespace UExplorer::Runtime
