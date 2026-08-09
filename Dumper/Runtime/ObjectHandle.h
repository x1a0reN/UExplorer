#pragma once

#include <cstdint>
#include <string>

namespace UExplorer::Runtime
{

enum class HandleError : std::uint8_t
{
	None,
	InvalidService,
	InvalidHandle,
	SessionMismatch,
	ContextGenerationMismatch,
	IdentityUnavailable,
	IdentitySourceInconsistent,
	SerialUnavailable,
	SerialMismatch,
	AddressMismatch,
	ClassFingerprintMismatch,
	FunctionOwnerMismatch,
	FunctionPathMismatch,
	FunctionSignatureMismatch
};

const char* ToString(HandleError error) noexcept;

struct ObjectIdentity
{
	std::int32_t Index = -1;
	std::int32_t SerialNumber = 0;
	std::uintptr_t Address = 0;
	std::uint64_t ClassFingerprint = 0;
};

struct FunctionIdentity
{
	ObjectIdentity Function;
	ObjectIdentity Owner;
	// Complete outer chain encoded as canonical raw FName identity tokens.
	std::string FullPath;
	std::uint64_t SignatureFingerprint = 0;
};

class IHandleIdentitySource
{
public:
	virtual ~IHandleIdentitySource() = default;
	virtual bool TryReadObject(std::int32_t index, ObjectIdentity& identity) = 0;
	virtual bool TryReadFunction(std::int32_t index, FunctionIdentity& identity) = 0;
};

struct ObjectHandle
{
	std::string SessionId;
	std::uint64_t ContextGeneration = 0;
	std::int32_t Index = -1;
	std::int32_t SerialNumber = 0;
	std::uintptr_t Address = 0;
	std::uint64_t ClassFingerprint = 0;
};

struct FunctionHandle
{
	ObjectHandle Function;
	ObjectHandle Owner;
	// Display names are metadata; only this canonical identity path is executable.
	std::string FullPath;
	std::uint64_t SignatureFingerprint = 0;
};

struct ObjectHandleResult
{
	HandleError Error = HandleError::None;
	ObjectHandle Value;

	bool Ok() const noexcept { return Error == HandleError::None; }
};

struct FunctionHandleResult
{
	HandleError Error = HandleError::None;
	FunctionHandle Value;

	bool Ok() const noexcept { return Error == HandleError::None; }
};

struct ObjectValidationResult
{
	HandleError Error = HandleError::None;
	ObjectIdentity Current;

	bool Ok() const noexcept { return Error == HandleError::None; }
};

struct FunctionValidationResult
{
	HandleError Error = HandleError::None;
	FunctionIdentity Current;

	bool Ok() const noexcept { return Error == HandleError::None; }
};

// The source must read one coherent game-thread view; this service never trusts
// an address or index supplied by the caller without re-reading live identity.
class ObjectHandleService final
{
public:
	ObjectHandleService(
		std::string sessionId,
		std::uint64_t contextGeneration,
		IHandleIdentitySource& source);
	ObjectHandleService(const ObjectHandleService&) = delete;
	ObjectHandleService& operator=(const ObjectHandleService&) = delete;

	bool IsConfigured() const noexcept;
	ObjectHandleResult IssueObject(std::int32_t index);
	ObjectValidationResult ValidateObject(const ObjectHandle& handle);
	FunctionHandleResult IssueFunction(std::int32_t index);
	FunctionValidationResult ValidateFunction(const FunctionHandle& handle);

private:
	HandleError ValidateEnvelope(const ObjectHandle& handle) const noexcept;
	HandleError ValidateIdentity(const ObjectIdentity& identity, std::int32_t expectedIndex) const noexcept;
	HandleError CompareIdentity(const ObjectHandle& handle, const ObjectIdentity& current) const noexcept;
	ObjectHandle MakeHandle(const ObjectIdentity& identity) const;
	bool TryReadObjectIdentity(std::int32_t index, ObjectIdentity& identity) noexcept;
	bool TryReadFunctionIdentity(std::int32_t index, FunctionIdentity& identity) noexcept;

	std::string m_SessionId;
	std::uint64_t m_ContextGeneration = 0;
	IHandleIdentitySource& m_Source;
};

} // namespace UExplorer::Runtime
