#include "ObjectHandle.h"

#include <cstddef>
#include <string_view>
#include <utility>

namespace UExplorer::Runtime
{

namespace
{

constexpr std::size_t kMaxSessionIdLength = 128;
constexpr std::size_t kMaxFunctionPathLength = 4096;

bool IsCanonicalFunctionIdentityPath(const std::string_view path) noexcept
{
	constexpr std::string_view prefix = "Function ";
	constexpr std::string_view tokenPrefix = "fname:";
	if (!path.starts_with(prefix) || path.size() > kMaxFunctionPathLength)
		return false;

	std::size_t cursor = prefix.size();
	std::size_t tokenCount = 0;
	for (;;)
	{
		if (!path.substr(cursor).starts_with(tokenPrefix))
			return false;
		cursor += tokenPrefix.size();
		const std::size_t hexBegin = cursor;
		while (cursor < path.size()
			&& ((path[cursor] >= '0' && path[cursor] <= '9')
				|| (path[cursor] >= 'a' && path[cursor] <= 'f')))
		{
			++cursor;
		}
		if (cursor == hexBegin || cursor >= path.size() || path[cursor++] != ':')
			return false;
		const std::size_t numberBegin = cursor;
		while (cursor < path.size() && path[cursor] >= '0' && path[cursor] <= '9')
			++cursor;
		if (cursor == numberBegin)
			return false;
		++tokenCount;
		if (cursor == path.size())
			return tokenCount >= 2;
		if (path[cursor++] != '.')
			return false;
	}
}

} // namespace

const char* ToString(const HandleError error) noexcept
{
	switch (error)
	{
	case HandleError::None: return "NONE";
	case HandleError::InvalidService: return "HANDLE_SERVICE_INVALID";
	case HandleError::InvalidHandle: return "HANDLE_INVALID";
	case HandleError::SessionMismatch: return "HANDLE_SESSION_MISMATCH";
	case HandleError::ContextGenerationMismatch: return "HANDLE_CONTEXT_GENERATION_MISMATCH";
	case HandleError::IdentityUnavailable: return "HANDLE_IDENTITY_UNAVAILABLE";
	case HandleError::IdentitySourceInconsistent: return "HANDLE_IDENTITY_SOURCE_INCONSISTENT";
	case HandleError::ExecutionThreadInvalid: return "HANDLE_EXECUTION_THREAD_INVALID";
	case HandleError::SerialUnavailable: return "HANDLE_SERIAL_UNAVAILABLE";
	case HandleError::SerialMismatch: return "HANDLE_SERIAL_MISMATCH";
	case HandleError::AddressMismatch: return "HANDLE_ADDRESS_MISMATCH";
	case HandleError::ClassFingerprintMismatch: return "HANDLE_CLASS_FINGERPRINT_MISMATCH";
	case HandleError::FunctionOwnerMismatch: return "FUNCTION_HANDLE_OWNER_MISMATCH";
	case HandleError::FunctionPathMismatch: return "FUNCTION_HANDLE_PATH_MISMATCH";
	case HandleError::FunctionSignatureMismatch: return "FUNCTION_HANDLE_SIGNATURE_MISMATCH";
	}
	return "HANDLE_UNKNOWN_ERROR";
}

ObjectHandleService::ObjectHandleService(
	std::string sessionId,
	const std::uint64_t contextGeneration,
	IHandleIdentitySource& source)
	: m_SessionId(std::move(sessionId)),
	  m_ContextGeneration(contextGeneration),
	  m_Source(source)
{
}

bool ObjectHandleService::IsConfigured() const noexcept
{
	return !m_SessionId.empty()
		&& m_SessionId.size() <= kMaxSessionIdLength
		&& m_ContextGeneration != 0
		&& m_Source.ContextGeneration() == m_ContextGeneration;
}

HandleError ObjectHandleService::ValidateIdentity(
	const ObjectIdentity& identity,
	const std::int32_t expectedIndex) const noexcept
{
	if (identity.Index != expectedIndex || expectedIndex < 0)
		return HandleError::IdentitySourceInconsistent;
	if (identity.SerialNumber <= 0)
		return HandleError::SerialUnavailable;
	if (identity.Address == 0 || identity.ClassFingerprint == 0)
		return HandleError::IdentityUnavailable;
	return HandleError::None;
}

HandleError ObjectHandleService::ValidateEnvelope(const ObjectHandle& handle) const noexcept
{
	if (!IsConfigured())
		return HandleError::InvalidService;
	if (handle.SessionId != m_SessionId)
		return HandleError::SessionMismatch;
	if (handle.ContextGeneration != m_ContextGeneration)
		return HandleError::ContextGenerationMismatch;
	if (handle.Index < 0 || handle.SerialNumber <= 0
		|| handle.Address == 0 || handle.ClassFingerprint == 0)
	{
		return HandleError::InvalidHandle;
	}
	return HandleError::None;
}

HandleError ObjectHandleService::CompareIdentity(
	const ObjectHandle& handle,
	const ObjectIdentity& current) const noexcept
{
	const HandleError identityError = ValidateIdentity(current, handle.Index);
	if (identityError != HandleError::None)
		return identityError;
	if (current.SerialNumber != handle.SerialNumber)
		return HandleError::SerialMismatch;
	if (current.Address != handle.Address)
		return HandleError::AddressMismatch;
	if (current.ClassFingerprint != handle.ClassFingerprint)
		return HandleError::ClassFingerprintMismatch;
	return HandleError::None;
}

ObjectHandle ObjectHandleService::MakeHandle(const ObjectIdentity& identity) const
{
	return {
		.SessionId = m_SessionId,
		.ContextGeneration = m_ContextGeneration,
		.Index = identity.Index,
		.SerialNumber = identity.SerialNumber,
		.Address = identity.Address,
		.ClassFingerprint = identity.ClassFingerprint
	};
}

bool ObjectHandleService::TryReadObjectIdentity(
	const std::int32_t index,
	ObjectIdentity& identity) noexcept
{
	try
	{
		return m_Source.TryReadObject(index, identity);
	}
	catch (...)
	{
		return false;
	}
}

bool ObjectHandleService::TryReadFunctionIdentity(
	const std::int32_t index,
	FunctionIdentity& identity) noexcept
{
	try
	{
		return m_Source.TryReadFunction(index, identity);
	}
	catch (...)
	{
		return false;
	}
}

ObjectHandleResult ObjectHandleService::IssueObject(const std::int32_t index)
{
	if (!IsConfigured())
		return {.Error = HandleError::InvalidService};
	if (!m_Source.IsCurrentExecutionThreadValid())
		return {.Error = HandleError::ExecutionThreadInvalid};
	ObjectIdentity identity;
	if (!TryReadObjectIdentity(index, identity))
		return {.Error = HandleError::IdentityUnavailable};
	const HandleError identityError = ValidateIdentity(identity, index);
	if (identityError != HandleError::None)
		return {.Error = identityError};
	return {.Value = MakeHandle(identity)};
}

ObjectValidationResult ObjectHandleService::ValidateObject(const ObjectHandle& handle)
{
	const HandleError envelopeError = ValidateEnvelope(handle);
	if (envelopeError != HandleError::None)
		return {.Error = envelopeError};
	if (!m_Source.IsCurrentExecutionThreadValid())
		return {.Error = HandleError::ExecutionThreadInvalid};
	ObjectIdentity current;
	if (!TryReadObjectIdentity(handle.Index, current))
		return {.Error = HandleError::IdentityUnavailable};
	return {.Error = CompareIdentity(handle, current), .Current = current};
}

FunctionHandleResult ObjectHandleService::IssueFunction(const std::int32_t index)
{
	if (!IsConfigured())
		return {.Error = HandleError::InvalidService};
	if (!m_Source.IsCurrentExecutionThreadValid())
		return {.Error = HandleError::ExecutionThreadInvalid};
	FunctionIdentity identity;
	if (!TryReadFunctionIdentity(index, identity))
		return {.Error = HandleError::IdentityUnavailable};
	HandleError identityError = ValidateIdentity(identity.Function, index);
	if (identityError != HandleError::None)
		return {.Error = identityError};
	identityError = ValidateIdentity(identity.Owner, identity.Owner.Index);
	if (identityError != HandleError::None)
		return {.Error = HandleError::FunctionOwnerMismatch};
	if (!IsCanonicalFunctionIdentityPath(identity.FullPath))
		return {.Error = HandleError::FunctionPathMismatch};
	if (identity.SignatureFingerprint == 0)
		return {.Error = HandleError::FunctionSignatureMismatch};

	return {
		.Value = {
			.Function = MakeHandle(identity.Function),
			.Owner = MakeHandle(identity.Owner),
			.FullPath = std::move(identity.FullPath),
			.SignatureFingerprint = identity.SignatureFingerprint
		}
	};
}

FunctionValidationResult ObjectHandleService::ValidateFunction(const FunctionHandle& handle)
{
	HandleError error = ValidateEnvelope(handle.Function);
	if (error != HandleError::None)
		return {.Error = error};
	error = ValidateEnvelope(handle.Owner);
	if (error != HandleError::None)
		return {.Error = HandleError::FunctionOwnerMismatch};
	if (!IsCanonicalFunctionIdentityPath(handle.FullPath)
		|| handle.SignatureFingerprint == 0)
	{
		return {.Error = HandleError::InvalidHandle};
	}
	if (!m_Source.IsCurrentExecutionThreadValid())
		return {.Error = HandleError::ExecutionThreadInvalid};

	FunctionIdentity current;
	if (!TryReadFunctionIdentity(handle.Function.Index, current))
		return {.Error = HandleError::IdentityUnavailable};
	error = CompareIdentity(handle.Function, current.Function);
	if (error != HandleError::None)
		return {.Error = error, .Current = std::move(current)};
	if (CompareIdentity(handle.Owner, current.Owner) != HandleError::None)
		return {.Error = HandleError::FunctionOwnerMismatch, .Current = std::move(current)};
	if (current.FullPath != handle.FullPath)
		return {.Error = HandleError::FunctionPathMismatch, .Current = std::move(current)};
	if (current.SignatureFingerprint != handle.SignatureFingerprint)
		return {.Error = HandleError::FunctionSignatureMismatch, .Current = std::move(current)};
	return {.Current = std::move(current)};
}

} // namespace UExplorer::Runtime
