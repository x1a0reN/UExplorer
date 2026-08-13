#include "BlueprintBytecodeEvidence.h"

#include <algorithm>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace UExplorer::Runtime
{
namespace
{

constexpr std::uint64_t kMaxProtocolGeneration = 9'007'199'254'740'991ULL;
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

bool IsBoundedText(const std::string_view value, const std::size_t maximum) noexcept
{
	return !value.empty() && value.size() <= maximum
		&& std::none_of(value.begin(), value.end(), [](const unsigned char character) {
			return character < 0x20 || character == 0x7F;
		});
}

bool IsValidBinding(const BlueprintEvidenceBinding& binding) noexcept
{
	return IsBoundedText(
			binding.SessionId,
			BlueprintBytecodeEvidenceStore::kMaxSessionBytes)
		&& binding.ContextGeneration > 0
		&& binding.ContextGeneration <= kMaxProtocolGeneration
		&& binding.ObjectSnapshotGeneration > 0
		&& binding.ObjectSnapshotGeneration <= kMaxProtocolGeneration
		&& binding.TypeSnapshotGeneration > 0
		&& binding.TypeSnapshotGeneration <= kMaxProtocolGeneration;
}

bool IsValidObjectHandle(
	const ObjectHandle& handle,
	const BlueprintEvidenceBinding& binding) noexcept
{
	return handle.SessionId == binding.SessionId
		&& handle.ContextGeneration == binding.ContextGeneration
		&& handle.Index >= 0
		&& handle.SerialNumber > 0
		&& handle.Address != 0
		&& handle.ClassFingerprint != 0;
}

bool IsValidFunctionHandle(
	const FunctionHandle& handle,
	const BlueprintEvidenceBinding& binding) noexcept
{
	return IsValidObjectHandle(handle.Function, binding)
		&& IsValidObjectHandle(handle.Owner, binding)
		&& IsBoundedText(handle.FullPath, BlueprintBytecodeEvidenceStore::kMaxPathBytes)
		&& handle.SignatureFingerprint != 0;
}

bool SameObjectHandle(const ObjectHandle& left, const ObjectHandle& right) noexcept
{
	return left.SessionId == right.SessionId
		&& left.ContextGeneration == right.ContextGeneration
		&& left.Index == right.Index
		&& left.SerialNumber == right.SerialNumber
		&& left.Address == right.Address
		&& left.ClassFingerprint == right.ClassFingerprint;
}

bool SameFunctionHandle(const FunctionHandle& left, const FunctionHandle& right) noexcept
{
	return SameObjectHandle(left.Function, right.Function)
		&& SameObjectHandle(left.Owner, right.Owner)
		&& left.FullPath == right.FullPath
		&& left.SignatureFingerprint == right.SignatureFingerprint;
}

bool IsIntegerWidth(const std::uint8_t width) noexcept
{
	return width == 1 || width == 2 || width == 4 || width == 8;
}

bool IsOptionalIntegerWidth(const std::uint8_t width) noexcept
{
	return width == 0 || IsIntegerWidth(width);
}

bool FitsLayoutField(
	const std::uint8_t offset,
	const std::uint8_t width,
	const std::uint8_t totalWidth) noexcept
{
	return width == 0
		|| static_cast<std::size_t>(offset) + static_cast<std::size_t>(width)
			<= static_cast<std::size_t>(totalWidth);
}

bool IsValidProfileDefinition(
	const BlueprintDecompiler::BytecodeProfile& profile) noexcept
{
	if (!IsBoundedText(profile.Id, BlueprintBytecodeEvidenceStore::kMaxProfileIdBytes)
		|| (profile.PointerWidth != 4 && profile.PointerWidth != 8)
		|| !IsIntegerWidth(profile.CodeSkipWidth)
		|| (profile.VectorComponentWidth != 4 && profile.VectorComponentWidth != 8)
		|| (profile.RotationComponentWidth != 4 && profile.RotationComponentWidth != 8)
		|| (profile.TransformComponentWidth != 4 && profile.TransformComponentWidth != 8))
	{
		return false;
	}

	const auto& name = profile.NameLayout;
	if (name.ByteWidth == 0 || name.ByteWidth > 32
		|| !IsOptionalIntegerWidth(name.ComparisonIndexWidth)
		|| !IsOptionalIntegerWidth(name.NumberWidth)
		|| !FitsLayoutField(
			name.ComparisonIndexOffset,
			name.ComparisonIndexWidth,
			name.ByteWidth)
		|| !FitsLayoutField(name.NumberOffset, name.NumberWidth, name.ByteWidth))
	{
		return false;
	}

	const auto& limits = profile.Limits;
	if (limits.MaxInputBytes == 0
		|| limits.MaxInputBytes > BlueprintBytecodeEvidenceStore::kMaxScriptBytes
		|| limits.MaxBytesConsumed == 0
		|| limits.MaxBytesConsumed > BlueprintBytecodeEvidenceStore::kMaxScriptBytes
		|| limits.MaxInstructions == 0
		|| limits.MaxInstructions > BlueprintBytecodeEvidenceStore::kMaxInstructions
		|| limits.MaxStringCodeUnits == 0
		|| limits.MaxStringCodeUnits > BlueprintBytecodeEvidenceStore::kMaxStringCodeUnits
		|| limits.MaxRecursionDepth == 0
		|| limits.MaxRecursionDepth > BlueprintBytecodeEvidenceStore::kMaxRecursionDepth)
	{
		return false;
	}

	bool hasEndOfScript = false;
	for (const BlueprintDecompiler::OpcodeMapping& mapping : profile.Opcodes)
	{
		switch (mapping.Semantic)
		{
		case BlueprintDecompiler::OpcodeSemantic::Unknown:
			if (mapping.Token != EExprToken::EX_Max)
				return false;
			break;
		case BlueprintDecompiler::OpcodeSemantic::ExprToken:
			if (mapping.Token == EExprToken::EX_Max)
				return false;
			hasEndOfScript = hasEndOfScript
				|| mapping.Token == EExprToken::EX_EndOfScript;
			break;
		case BlueprintDecompiler::OpcodeSemantic::PrimitiveCast:
			if (mapping.Token != EExprToken::EX_Max)
				return false;
			break;
		default:
			return false;
		}
	}
	return hasEndOfScript;
}

bool IsValidCapture(const BlueprintBytecodeCapture& capture) noexcept
{
	return IsValidBinding(capture.Binding)
		&& IsValidFunctionHandle(capture.Function, capture.Binding)
		&& IsBoundedText(
			capture.FunctionPath,
			BlueprintBytecodeEvidenceStore::kMaxPathBytes)
		&& IsBoundedText(capture.Source, BlueprintBytecodeEvidenceStore::kMaxSourceBytes)
		&& capture.ScriptFieldOffset > 0
		&& capture.ScriptFieldOffset
			<= BlueprintBytecodeEvidenceStore::kMaxScriptFieldOffset
		&& capture.ScriptDataAddress != 0
		&& capture.ScriptNum > 0
		&& capture.ScriptMax >= capture.ScriptNum
		&& capture.ScriptMax <= BlueprintBytecodeEvidenceStore::kMaxScriptCapacity
		&& !capture.Script.empty()
		&& capture.Script.size() <= BlueprintBytecodeEvidenceStore::kMaxScriptBytes
		&& capture.Script.size() == static_cast<std::size_t>(capture.ScriptNum)
		&& capture.ScriptDataAddress
			<= (std::numeric_limits<std::uintptr_t>::max)() - capture.Script.size()
		&& capture.CapturedAtMonotonicUs != 0
		&& capture.HeaderWitnessFingerprint != 0
		&& capture.HeaderWitnessFingerprint
			== ComputeBlueprintScriptHeaderFingerprint(capture)
		&& capture.EvidenceFingerprint != 0
		&& capture.EvidenceFingerprint
			== ComputeBlueprintBytecodeCaptureFingerprint(capture);
}

bool IsValidProfile(const BlueprintBytecodeProfileRecord& profile) noexcept
{
	return IsValidBinding(profile.Binding)
		&& IsBoundedText(profile.Source, BlueprintBytecodeEvidenceStore::kMaxSourceBytes)
		&& IsValidProfileDefinition(profile.Definition)
		&& profile.EvidenceFingerprint != 0
		&& profile.EvidenceFingerprint
			== ComputeBlueprintBytecodeProfileFingerprint(profile);
}

void AppendByte(std::uint64_t& hash, const std::uint8_t value) noexcept
{
	hash ^= value;
	hash *= kFnvPrime;
}

template<typename T>
void AppendUnsigned(std::uint64_t& hash, const T value) noexcept
{
	static_assert(std::is_unsigned_v<T>);
	for (std::size_t index = 0; index < sizeof(T); ++index)
	{
		AppendByte(
			hash,
			static_cast<std::uint8_t>(value >> (index * 8U)));
	}
}

void AppendSigned(std::uint64_t& hash, const std::int32_t value) noexcept
{
	AppendUnsigned(hash, static_cast<std::uint32_t>(value));
}

void AppendText(std::uint64_t& hash, const std::string_view value) noexcept
{
	AppendUnsigned(hash, static_cast<std::uint64_t>(value.size()));
	for (const unsigned char character : value)
		AppendByte(hash, character);
}

void AppendBinding(std::uint64_t& hash, const BlueprintEvidenceBinding& binding) noexcept
{
	AppendText(hash, binding.SessionId);
	AppendUnsigned(hash, binding.ContextGeneration);
	AppendUnsigned(hash, binding.ObjectSnapshotGeneration);
	AppendUnsigned(hash, binding.TypeSnapshotGeneration);
}

void AppendObjectHandle(std::uint64_t& hash, const ObjectHandle& handle) noexcept
{
	AppendText(hash, handle.SessionId);
	AppendUnsigned(hash, handle.ContextGeneration);
	AppendSigned(hash, handle.Index);
	AppendSigned(hash, handle.SerialNumber);
	AppendUnsigned(hash, static_cast<std::uint64_t>(handle.Address));
	AppendUnsigned(hash, handle.ClassFingerprint);
}

void AppendFunctionHandle(std::uint64_t& hash, const FunctionHandle& handle) noexcept
{
	AppendObjectHandle(hash, handle.Function);
	AppendObjectHandle(hash, handle.Owner);
	AppendText(hash, handle.FullPath);
	AppendUnsigned(hash, handle.SignatureFingerprint);
}

std::uint64_t FinishFingerprint(const std::uint64_t hash) noexcept
{
	return hash == 0 ? 1 : hash;
}

} // namespace

bool SameBlueprintEvidenceBinding(
	const BlueprintEvidenceBinding& left,
	const BlueprintEvidenceBinding& right) noexcept
{
	return left.SessionId == right.SessionId
		&& left.ContextGeneration == right.ContextGeneration
		&& left.ObjectSnapshotGeneration == right.ObjectSnapshotGeneration
		&& left.TypeSnapshotGeneration == right.TypeSnapshotGeneration;
}

std::uint64_t ComputeBlueprintScriptHeaderFingerprint(
	const BlueprintBytecodeCapture& capture) noexcept
{
	std::uint64_t hash = kFnvOffset;
	AppendText(hash, "UExplorer.BlueprintScriptHeader.v1");
	AppendBinding(hash, capture.Binding);
	AppendFunctionHandle(hash, capture.Function);
	AppendUnsigned(hash, capture.ScriptFieldOffset);
	AppendUnsigned(hash, static_cast<std::uint64_t>(capture.ScriptDataAddress));
	AppendSigned(hash, capture.ScriptNum);
	AppendSigned(hash, capture.ScriptMax);
	return FinishFingerprint(hash);
}

std::uint64_t ComputeBlueprintBytecodeCaptureFingerprint(
	const BlueprintBytecodeCapture& capture) noexcept
{
	std::uint64_t hash = kFnvOffset;
	AppendText(hash, "UExplorer.BlueprintBytecodeCapture.v1");
	AppendBinding(hash, capture.Binding);
	AppendFunctionHandle(hash, capture.Function);
	AppendText(hash, capture.FunctionPath);
	AppendText(hash, capture.Source);
	AppendUnsigned(hash, capture.ScriptFieldOffset);
	AppendUnsigned(hash, static_cast<std::uint64_t>(capture.ScriptDataAddress));
	AppendSigned(hash, capture.ScriptNum);
	AppendSigned(hash, capture.ScriptMax);
	AppendUnsigned(hash, capture.HeaderWitnessFingerprint);
	AppendUnsigned(hash, capture.CapturedAtMonotonicUs);
	AppendUnsigned(hash, static_cast<std::uint64_t>(capture.Script.size()));
	for (const std::uint8_t value : capture.Script)
		AppendByte(hash, value);
	return FinishFingerprint(hash);
}

std::uint64_t ComputeBlueprintBytecodeProfileFingerprint(
	const BlueprintBytecodeProfileRecord& profile) noexcept
{
	std::uint64_t hash = kFnvOffset;
	AppendText(hash, "UExplorer.BlueprintBytecodeProfile.v1");
	AppendBinding(hash, profile.Binding);
	AppendText(hash, profile.Source);
	const auto& definition = profile.Definition;
	AppendText(hash, definition.Id);
	AppendByte(hash, definition.PointerWidth);
	AppendByte(hash, definition.CodeSkipWidth);
	AppendByte(hash, definition.VectorComponentWidth);
	AppendByte(hash, definition.RotationComponentWidth);
	AppendByte(hash, definition.TransformComponentWidth);
	AppendByte(hash, definition.NameLayout.ByteWidth);
	AppendByte(hash, definition.NameLayout.ComparisonIndexOffset);
	AppendByte(hash, definition.NameLayout.ComparisonIndexWidth);
	AppendByte(hash, definition.NameLayout.NumberOffset);
	AppendByte(hash, definition.NameLayout.NumberWidth);
	AppendUnsigned(hash, static_cast<std::uint64_t>(definition.Limits.MaxInputBytes));
	AppendUnsigned(hash, static_cast<std::uint64_t>(definition.Limits.MaxBytesConsumed));
	AppendUnsigned(hash, static_cast<std::uint64_t>(definition.Limits.MaxInstructions));
	AppendUnsigned(hash, static_cast<std::uint64_t>(definition.Limits.MaxStringCodeUnits));
	AppendUnsigned(hash, definition.Limits.MaxRecursionDepth);
	for (const BlueprintDecompiler::OpcodeMapping& mapping : definition.Opcodes)
	{
		AppendByte(hash, static_cast<std::uint8_t>(mapping.Semantic));
		AppendByte(hash, static_cast<std::uint8_t>(mapping.Token));
	}
	return FinishFingerprint(hash);
}

const char* ToString(const BlueprintEvidenceSourceError error) noexcept
{
	switch (error)
	{
	case BlueprintEvidenceSourceError::None: return "NONE";
	case BlueprintEvidenceSourceError::Unavailable: return "BLUEPRINT_EVIDENCE_UNAVAILABLE";
	case BlueprintEvidenceSourceError::Stopped: return "BLUEPRINT_EVIDENCE_SOURCE_STOPPED";
	case BlueprintEvidenceSourceError::InvalidRequest: return "BLUEPRINT_EVIDENCE_REQUEST_INVALID";
	case BlueprintEvidenceSourceError::DependencyChanged: return "BLUEPRINT_EVIDENCE_DEPENDENCY_CHANGED";
	case BlueprintEvidenceSourceError::FunctionHandleStale: return "BLUEPRINT_FUNCTION_HANDLE_STALE";
	case BlueprintEvidenceSourceError::ExecutionThreadInvalid: return "BLUEPRINT_EXECUTION_THREAD_INVALID";
	case BlueprintEvidenceSourceError::LimitExceeded: return "BLUEPRINT_EVIDENCE_LIMIT_EXCEEDED";
	case BlueprintEvidenceSourceError::InvalidEvidence: return "BLUEPRINT_EVIDENCE_INVALID";
	case BlueprintEvidenceSourceError::AllocationFailed: return "BLUEPRINT_EVIDENCE_ALLOCATION_FAILED";
	case BlueprintEvidenceSourceError::InternalError: return "BLUEPRINT_EVIDENCE_INTERNAL_ERROR";
	}
	return "BLUEPRINT_EVIDENCE_UNKNOWN_ERROR";
}

const char* ToString(const BlueprintEvidencePublishError error) noexcept
{
	switch (error)
	{
	case BlueprintEvidencePublishError::None: return "NONE";
	case BlueprintEvidencePublishError::StoreInvalid: return "BLUEPRINT_EVIDENCE_STORE_INVALID";
	case BlueprintEvidencePublishError::StoreStopped: return "BLUEPRINT_EVIDENCE_STORE_STOPPED";
	case BlueprintEvidencePublishError::BindingMismatch: return "BLUEPRINT_EVIDENCE_BINDING_MISMATCH";
	case BlueprintEvidencePublishError::RecordInvalid: return "BLUEPRINT_EVIDENCE_RECORD_INVALID";
	case BlueprintEvidencePublishError::FingerprintMismatch: return "BLUEPRINT_EVIDENCE_FINGERPRINT_MISMATCH";
	case BlueprintEvidencePublishError::DuplicateConflict: return "BLUEPRINT_EVIDENCE_DUPLICATE_CONFLICT";
	case BlueprintEvidencePublishError::AllocationFailed: return "BLUEPRINT_EVIDENCE_ALLOCATION_FAILED";
	}
	return "BLUEPRINT_EVIDENCE_UNKNOWN_ERROR";
}

BlueprintBytecodeEvidenceStore::BlueprintBytecodeEvidenceStore(
	BlueprintEvidenceBinding binding)
	: m_Binding(std::move(binding))
{
}

bool BlueprintBytecodeEvidenceStore::IsConfigured() const noexcept
{
	return IsValidBinding(m_Binding);
}

BlueprintEvidencePublishError BlueprintBytecodeEvidenceStore::PublishCapture(
	BlueprintBytecodeCapture capture) noexcept
{
	try
	{
		if (!IsConfigured())
			return BlueprintEvidencePublishError::StoreInvalid;
		if (IsStopped())
			return BlueprintEvidencePublishError::StoreStopped;
		if (!SameBlueprintEvidenceBinding(capture.Binding, m_Binding))
			return BlueprintEvidencePublishError::BindingMismatch;
		if (capture.EvidenceFingerprint == 0
			|| capture.EvidenceFingerprint
				!= ComputeBlueprintBytecodeCaptureFingerprint(capture))
		{
			return BlueprintEvidencePublishError::FingerprintMismatch;
		}
		if (!IsValidCapture(capture))
			return BlueprintEvidencePublishError::RecordInvalid;

		auto record = std::make_shared<const BlueprintBytecodeCapture>(std::move(capture));
		std::lock_guard lock(m_Mutex);
		if (IsStopped())
			return BlueprintEvidencePublishError::StoreStopped;
		if (m_Captures.contains(record->FunctionPath))
			return BlueprintEvidencePublishError::DuplicateConflict;
		m_Captures.emplace(record->FunctionPath, std::move(record));
		return BlueprintEvidencePublishError::None;
	}
	catch (const std::bad_alloc&)
	{
		return BlueprintEvidencePublishError::AllocationFailed;
	}
	catch (...)
	{
		return BlueprintEvidencePublishError::RecordInvalid;
	}
}

BlueprintEvidencePublishError BlueprintBytecodeEvidenceStore::PublishProfile(
	BlueprintBytecodeProfileRecord profile) noexcept
{
	try
	{
		if (!IsConfigured())
			return BlueprintEvidencePublishError::StoreInvalid;
		if (IsStopped())
			return BlueprintEvidencePublishError::StoreStopped;
		if (!SameBlueprintEvidenceBinding(profile.Binding, m_Binding))
			return BlueprintEvidencePublishError::BindingMismatch;
		if (profile.EvidenceFingerprint == 0
			|| profile.EvidenceFingerprint
				!= ComputeBlueprintBytecodeProfileFingerprint(profile))
		{
			return BlueprintEvidencePublishError::FingerprintMismatch;
		}
		if (!IsValidProfile(profile))
			return BlueprintEvidencePublishError::RecordInvalid;

		auto record = std::make_shared<const BlueprintBytecodeProfileRecord>(std::move(profile));
		std::lock_guard lock(m_Mutex);
		if (IsStopped())
			return BlueprintEvidencePublishError::StoreStopped;
		if (m_Profiles.contains(record->Definition.Id))
			return BlueprintEvidencePublishError::DuplicateConflict;
		m_Profiles.emplace(record->Definition.Id, std::move(record));
		return BlueprintEvidencePublishError::None;
	}
	catch (const std::bad_alloc&)
	{
		return BlueprintEvidencePublishError::AllocationFailed;
	}
	catch (...)
	{
		return BlueprintEvidencePublishError::RecordInvalid;
	}
}

BlueprintBytecodeCaptureResult BlueprintBytecodeEvidenceStore::Capture(
	const BlueprintBytecodeCaptureRequest& request)
{
	try
	{
		if (!IsConfigured() || IsStopped())
		{
			return {.Error = IsStopped()
				? BlueprintEvidenceSourceError::Stopped
				: BlueprintEvidenceSourceError::Unavailable};
		}
		if (!SameBlueprintEvidenceBinding(request.Binding, m_Binding))
			return {.Error = BlueprintEvidenceSourceError::DependencyChanged};
		if (!IsValidFunctionHandle(request.Function, request.Binding)
			|| !IsBoundedText(request.FunctionPath, kMaxPathBytes)
			|| request.MaxScriptBytes == 0
			|| request.MaxScriptBytes > kMaxScriptBytes)
		{
			return {.Error = BlueprintEvidenceSourceError::InvalidRequest};
		}

		std::lock_guard lock(m_Mutex);
		const auto found = m_Captures.find(request.FunctionPath);
		if (found == m_Captures.end())
			return {.Error = BlueprintEvidenceSourceError::Unavailable};
		if (!SameFunctionHandle(found->second->Function, request.Function))
			return {.Error = BlueprintEvidenceSourceError::FunctionHandleStale};
		if (found->second->Script.size() > request.MaxScriptBytes)
			return {.Error = BlueprintEvidenceSourceError::LimitExceeded};
		return {
			.Error = BlueprintEvidenceSourceError::None,
			.Capture = found->second
		};
	}
	catch (const std::bad_alloc&)
	{
		return {.Error = BlueprintEvidenceSourceError::AllocationFailed};
	}
	catch (...)
	{
		return {.Error = BlueprintEvidenceSourceError::InternalError};
	}
}

BlueprintBytecodeProfileResult BlueprintBytecodeEvidenceStore::ResolveProfile(
	const BlueprintBytecodeProfileRequest& request) const
{
	try
	{
		if (!IsConfigured() || IsStopped())
		{
			return {.Error = IsStopped()
				? BlueprintEvidenceSourceError::Stopped
				: BlueprintEvidenceSourceError::Unavailable};
		}
		if (!SameBlueprintEvidenceBinding(request.Binding, m_Binding))
			return {.Error = BlueprintEvidenceSourceError::DependencyChanged};
		if (!IsBoundedText(request.ProfileId, kMaxProfileIdBytes))
			return {.Error = BlueprintEvidenceSourceError::InvalidRequest};

		std::lock_guard lock(m_Mutex);
		const auto found = m_Profiles.find(request.ProfileId);
		if (found == m_Profiles.end())
			return {.Error = BlueprintEvidenceSourceError::Unavailable};
		return {
			.Error = BlueprintEvidenceSourceError::None,
			.Profile = found->second
		};
	}
	catch (const std::bad_alloc&)
	{
		return {.Error = BlueprintEvidenceSourceError::AllocationFailed};
	}
	catch (...)
	{
		return {.Error = BlueprintEvidenceSourceError::InternalError};
	}
}

void BlueprintBytecodeEvidenceStore::Stop() noexcept
{
	m_Stopped.store(true, std::memory_order_release);
	try
	{
		std::lock_guard lock(m_Mutex);
		m_Captures.clear();
		m_Profiles.clear();
	}
	catch (...)
	{
	}
}

} // namespace UExplorer::Runtime
