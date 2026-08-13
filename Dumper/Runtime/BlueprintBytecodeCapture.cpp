#include "BlueprintBytecodeCapture.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <new>
#include <span>
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
		&& IsBoundedText(
			handle.FullPath,
			BlueprintBytecodeEvidenceStore::kMaxPathBytes)
		&& handle.SignatureFingerprint != 0;
}

bool IsValidRequest(const BlueprintBytecodeCaptureRequest& request) noexcept
{
	return IsValidBinding(request.Binding)
		&& IsValidFunctionHandle(request.Function, request.Binding)
		&& IsBoundedText(
			request.FunctionPath,
			BlueprintBytecodeEvidenceStore::kMaxPathBytes)
		&& request.MaxScriptBytes > 0
		&& request.MaxScriptBytes
			<= BlueprintBytecodeEvidenceStore::kMaxScriptBytes;
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

bool SameFunctionHandle(
	const FunctionHandle& left,
	const FunctionHandle& right) noexcept
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

bool FieldFits(
	const std::uint32_t offset,
	const std::uint8_t width,
	const std::uint32_t headerWidth) noexcept
{
	return width > 0
		&& static_cast<std::uint64_t>(offset) + width <= headerWidth;
}

bool FieldsOverlap(
	const std::uint32_t leftOffset,
	const std::uint8_t leftWidth,
	const std::uint32_t rightOffset,
	const std::uint8_t rightWidth) noexcept
{
	const std::uint64_t leftEnd = static_cast<std::uint64_t>(leftOffset) + leftWidth;
	const std::uint64_t rightEnd = static_cast<std::uint64_t>(rightOffset) + rightWidth;
	return leftOffset < rightEnd && rightOffset < leftEnd;
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

void AppendText(std::uint64_t& hash, const std::string_view value) noexcept
{
	AppendUnsigned(hash, static_cast<std::uint64_t>(value.size()));
	for (const unsigned char character : value)
		AppendByte(hash, character);
}

void AppendBinding(
	std::uint64_t& hash,
	const BlueprintEvidenceBinding& binding) noexcept
{
	AppendText(hash, binding.SessionId);
	AppendUnsigned(hash, binding.ContextGeneration);
	AppendUnsigned(hash, binding.ObjectSnapshotGeneration);
	AppendUnsigned(hash, binding.TypeSnapshotGeneration);
}

std::uint64_t FinishFingerprint(const std::uint64_t hash) noexcept
{
	return hash == 0 ? 1 : hash;
}

bool TryDecodeUnsignedLittleEndian(
	const std::span<const std::byte> bytes,
	const std::uint32_t offset,
	const std::uint8_t width,
	std::uint64_t& value) noexcept
{
	value = 0;
	if (!IsIntegerWidth(width)
		|| static_cast<std::uint64_t>(offset) + width > bytes.size())
	{
		return false;
	}
	for (std::uint8_t index = 0; index < width; ++index)
	{
		value |= static_cast<std::uint64_t>(
			std::to_integer<std::uint8_t>(bytes[offset + index]))
			<< (index * 8U);
	}
	return true;
}

bool TryDecodeSignedLittleEndian(
	const std::span<const std::byte> bytes,
	const std::uint32_t offset,
	const std::uint8_t width,
	std::int64_t& value) noexcept
{
	std::uint64_t raw = 0;
	if (!TryDecodeUnsignedLittleEndian(bytes, offset, width, raw))
		return false;
	const std::uint32_t bits = static_cast<std::uint32_t>(width) * 8U;
	const std::uint64_t sign = std::uint64_t{1} << (bits - 1U);
	if ((raw & sign) == 0)
	{
		value = static_cast<std::int64_t>(raw);
		return true;
	}
	if (bits == 64U)
	{
		if (raw == sign)
		{
			value = (std::numeric_limits<std::int64_t>::min)();
			return true;
		}
		const std::uint64_t magnitude = (~raw) + 1U;
		if (magnitude > static_cast<std::uint64_t>(
			(std::numeric_limits<std::int64_t>::max)()))
		{
			return false;
		}
		value = -static_cast<std::int64_t>(magnitude);
		return true;
	}
	const std::uint64_t mask = (std::uint64_t{1} << bits) - 1U;
	const std::uint64_t magnitude = ((~raw) & mask) + 1U;
	value = -static_cast<std::int64_t>(magnitude);
	return true;
}

struct ScriptArrayHeader
{
	std::uintptr_t Data = 0;
	std::int32_t Num = 0;
	std::int32_t Max = 0;
};

bool TryDecodeHeader(
	const std::span<const std::byte> bytes,
	const BlueprintScriptArrayLayoutWitness& layout,
	ScriptArrayHeader& header) noexcept
{
	header = {};
	std::uint64_t pointer = 0;
	std::int64_t num = 0;
	std::int64_t max = 0;
	if (!TryDecodeUnsignedLittleEndian(
			bytes,
			layout.DataPointerOffset,
			layout.DataPointerWidth,
			pointer)
		|| !TryDecodeSignedLittleEndian(
			bytes,
			layout.NumOffset,
			layout.NumWidth,
			num)
		|| !TryDecodeSignedLittleEndian(
			bytes,
			layout.MaxOffset,
			layout.MaxWidth,
			max)
		|| pointer > (std::numeric_limits<std::uintptr_t>::max)()
		|| num < (std::numeric_limits<std::int32_t>::min)()
		|| num > (std::numeric_limits<std::int32_t>::max)()
		|| max < (std::numeric_limits<std::int32_t>::min)()
		|| max > (std::numeric_limits<std::int32_t>::max)())
	{
		return false;
	}
	header.Data = static_cast<std::uintptr_t>(pointer);
	header.Num = static_cast<std::int32_t>(num);
	header.Max = static_cast<std::int32_t>(max);
	return true;
}

std::uint64_t MonotonicMicroseconds() noexcept
{
	const auto value = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
	return value > 0 ? static_cast<std::uint64_t>(value) : 1;
}

BlueprintEvidenceSourceError MapSourceError(
	const BlueprintBytecodeLiveCaptureError error) noexcept
{
	switch (error)
	{
	case BlueprintBytecodeLiveCaptureError::None:
		return BlueprintEvidenceSourceError::None;
	case BlueprintBytecodeLiveCaptureError::SourceStopped:
	case BlueprintBytecodeLiveCaptureError::EvidenceStoreStopped:
	case BlueprintBytecodeLiveCaptureError::ExecutorCancelled:
		return BlueprintEvidenceSourceError::Stopped;
	case BlueprintBytecodeLiveCaptureError::RequestInvalid:
		return BlueprintEvidenceSourceError::InvalidRequest;
	case BlueprintBytecodeLiveCaptureError::BindingMismatch:
	case BlueprintBytecodeLiveCaptureError::DependencyChanged:
	case BlueprintBytecodeLiveCaptureError::FunctionMetadataMismatch:
	case BlueprintBytecodeLiveCaptureError::HeaderChangedDuringCopy:
		return BlueprintEvidenceSourceError::DependencyChanged;
	case BlueprintBytecodeLiveCaptureError::ExecutionThreadInvalid:
	case BlueprintBytecodeLiveCaptureError::ExecutorWaitDenied:
		return BlueprintEvidenceSourceError::ExecutionThreadInvalid;
	case BlueprintBytecodeLiveCaptureError::FunctionHandleStale:
		return BlueprintEvidenceSourceError::FunctionHandleStale;
	case BlueprintBytecodeLiveCaptureError::ScriptLimitExceeded:
		return BlueprintEvidenceSourceError::LimitExceeded;
	case BlueprintBytecodeLiveCaptureError::LayoutInvalid:
	case BlueprintBytecodeLiveCaptureError::HeaderAddressOverflow:
	case BlueprintBytecodeLiveCaptureError::HeaderReadFailed:
	case BlueprintBytecodeLiveCaptureError::HeaderInvalid:
	case BlueprintBytecodeLiveCaptureError::ScriptAddressInvalid:
	case BlueprintBytecodeLiveCaptureError::ScriptReadFailed:
	case BlueprintBytecodeLiveCaptureError::ScriptTerminatorInvalid:
	case BlueprintBytecodeLiveCaptureError::PublishFailed:
		return BlueprintEvidenceSourceError::InvalidEvidence;
	case BlueprintBytecodeLiveCaptureError::AllocationFailed:
		return BlueprintEvidenceSourceError::AllocationFailed;
	case BlueprintBytecodeLiveCaptureError::InternalError:
	case BlueprintBytecodeLiveCaptureError::ExecutorFailed:
		return BlueprintEvidenceSourceError::InternalError;
	case BlueprintBytecodeLiveCaptureError::SourceInvalid:
	case BlueprintBytecodeLiveCaptureError::EvidenceUnavailable:
	case BlueprintBytecodeLiveCaptureError::LayoutUnavailable:
	case BlueprintBytecodeLiveCaptureError::DependencyUnavailable:
	case BlueprintBytecodeLiveCaptureError::ExecutorDisabled:
	case BlueprintBytecodeLiveCaptureError::ExecutorQueueBusy:
	case BlueprintBytecodeLiveCaptureError::ExecutorTimedOut:
		return BlueprintEvidenceSourceError::Unavailable;
	}
	return BlueprintEvidenceSourceError::InternalError;
}

BlueprintBytecodeLiveCaptureError MapLookupError(
	const BlueprintEvidenceSourceError error) noexcept
{
	switch (error)
	{
	case BlueprintEvidenceSourceError::None:
		return BlueprintBytecodeLiveCaptureError::None;
	case BlueprintEvidenceSourceError::Unavailable:
		return BlueprintBytecodeLiveCaptureError::EvidenceUnavailable;
	case BlueprintEvidenceSourceError::Stopped:
		return BlueprintBytecodeLiveCaptureError::EvidenceStoreStopped;
	case BlueprintEvidenceSourceError::InvalidRequest:
		return BlueprintBytecodeLiveCaptureError::RequestInvalid;
	case BlueprintEvidenceSourceError::DependencyChanged:
		return BlueprintBytecodeLiveCaptureError::BindingMismatch;
	case BlueprintEvidenceSourceError::FunctionHandleStale:
		return BlueprintBytecodeLiveCaptureError::FunctionHandleStale;
	case BlueprintEvidenceSourceError::ExecutionThreadInvalid:
		return BlueprintBytecodeLiveCaptureError::ExecutionThreadInvalid;
	case BlueprintEvidenceSourceError::LimitExceeded:
		return BlueprintBytecodeLiveCaptureError::ScriptLimitExceeded;
	case BlueprintEvidenceSourceError::InvalidEvidence:
		return BlueprintBytecodeLiveCaptureError::LayoutInvalid;
	case BlueprintEvidenceSourceError::AllocationFailed:
		return BlueprintBytecodeLiveCaptureError::AllocationFailed;
	case BlueprintEvidenceSourceError::InternalError:
		return BlueprintBytecodeLiveCaptureError::InternalError;
	}
	return BlueprintBytecodeLiveCaptureError::InternalError;
}

BlueprintBytecodeLiveCaptureError MapSubmitResult(
	const GameThreadSubmitResult result) noexcept
{
	switch (result)
	{
	case GameThreadSubmitResult::Completed:
		return BlueprintBytecodeLiveCaptureError::None;
	case GameThreadSubmitResult::Disabled:
		return BlueprintBytecodeLiveCaptureError::ExecutorDisabled;
	case GameThreadSubmitResult::Cancelled:
		return BlueprintBytecodeLiveCaptureError::ExecutorCancelled;
	case GameThreadSubmitResult::PumpThreadWaitDenied:
		return BlueprintBytecodeLiveCaptureError::ExecutorWaitDenied;
	case GameThreadSubmitResult::QueueBusy:
		return BlueprintBytecodeLiveCaptureError::ExecutorQueueBusy;
	case GameThreadSubmitResult::TimedOutBeforeStart:
	case GameThreadSubmitResult::TimedOutWhileRunning:
		return BlueprintBytecodeLiveCaptureError::ExecutorTimedOut;
	case GameThreadSubmitResult::ExecutionFailed:
		return BlueprintBytecodeLiveCaptureError::ExecutorFailed;
	}
	return BlueprintBytecodeLiveCaptureError::ExecutorFailed;
}

class BlueprintBytecodeCaptureWork final : public IGameThreadWork
{
public:
	BlueprintBytecodeCaptureWork(
		EngineFacade& engine,
		std::shared_ptr<const EngineSnapshot> objects,
		std::shared_ptr<const TypeSnapshot> types,
		BlueprintScriptArrayLayoutWitness layout,
		BlueprintBytecodeCaptureRequest request,
		CallbackBarrier::Lease sourceLease)
		: m_SourceLease(std::move(sourceLease)),
		  m_Engine(engine),
		  m_Objects(std::move(objects)),
		  m_Types(std::move(types)),
		  m_Layout(std::move(layout)),
		  m_Request(std::move(request))
	{
		m_Capture.Binding = m_Request.Binding;
		m_Capture.Function = m_Request.Function;
		m_Capture.FunctionPath = m_Request.FunctionPath;
		m_Capture.Source = m_Layout.Source;
		m_Capture.ScriptFieldOffset = m_Layout.ScriptFieldOffset;
		m_Capture.Script.resize(m_Request.MaxScriptBytes);
	}

	bool Execute() override
	{
		try
		{
			return ExecuteBounded();
		}
		catch (const std::bad_alloc&)
		{
			m_Error = BlueprintBytecodeLiveCaptureError::AllocationFailed;
			return true;
		}
		catch (...)
		{
			m_Error = BlueprintBytecodeLiveCaptureError::InternalError;
			return true;
		}
	}

	BlueprintBytecodeLiveCaptureError Error() const noexcept { return m_Error; }
	HandleError HandleValidationError() const noexcept { return m_HandleError; }
	MemoryError MemoryReadError() const noexcept { return m_MemoryError; }
	BlueprintBytecodeCapture TakeCapture() noexcept { return std::move(m_Capture); }

private:
	bool DependenciesCurrent() const noexcept
	{
		return m_Objects && m_Types
			&& m_Engine.SessionId() == m_Request.Binding.SessionId
			&& m_Engine.ContextGeneration() == m_Request.Binding.ContextGeneration
			&& m_Objects->SessionId == m_Request.Binding.SessionId
			&& m_Objects->ContextGeneration == m_Request.Binding.ContextGeneration
			&& m_Objects->Generation == m_Request.Binding.ObjectSnapshotGeneration
			&& m_Types->SessionId() == m_Request.Binding.SessionId
			&& m_Types->ContextGeneration() == m_Request.Binding.ContextGeneration
			&& m_Types->ObjectSnapshotGeneration()
				== m_Request.Binding.ObjectSnapshotGeneration
			&& m_Types->Generation() == m_Request.Binding.TypeSnapshotGeneration
			&& m_Engine.Snapshots().Current() == m_Objects
			&& m_Engine.Types().Current() == m_Types;
	}

	bool MetadataMatches() const noexcept
	{
		const EngineSnapshotObject* functionRecord =
			m_Objects->FindByIndex(m_Request.Function.Function.Index);
		const EngineSnapshotObject* ownerRecord =
			m_Objects->FindByIndex(m_Request.Function.Owner.Index);
		const ReflectedFunctionLookup lookup =
			m_Types->FindFunctionByFullPath(m_Request.FunctionPath);
		return functionRecord && ownerRecord
			&& functionRecord->Kind == EngineObjectKind::Function
			&& SameObjectHandle(functionRecord->Handle, m_Request.Function.Function)
			&& SameObjectHandle(ownerRecord->Handle, m_Request.Function.Owner)
			&& lookup.Found()
			&& lookup.Function->FullPath == m_Request.FunctionPath
			&& SameFunctionHandle(lookup.Function->Handle, m_Request.Function)
			&& SameObjectHandle(
				lookup.DeclaringType->Handle,
				m_Request.Function.Owner)
			&& ownerRecord->FullPath == lookup.DeclaringType->FullPath;
	}

	bool ValidateFunctionIdentity()
	{
		const FunctionValidationResult validation =
			m_Engine.ValidateFunctionHandle(m_Request.Function);
		if (validation.Ok())
			return true;
		m_Error = validation.Error == HandleError::ExecutionThreadInvalid
			? BlueprintBytecodeLiveCaptureError::ExecutionThreadInvalid
			: BlueprintBytecodeLiveCaptureError::FunctionHandleStale;
		m_HandleError = validation.Error;
		return false;
	}

	bool ExecuteBounded()
	{
		if (!DependenciesCurrent())
		{
			m_Error = BlueprintBytecodeLiveCaptureError::DependencyChanged;
			return true;
		}
		if (!m_Engine.IsCurrentExecutionThreadValid())
		{
			m_Error = BlueprintBytecodeLiveCaptureError::ExecutionThreadInvalid;
			return true;
		}
		if (!ValidateFunctionIdentity())
			return true;
		if (!MetadataMatches())
		{
			m_Error = BlueprintBytecodeLiveCaptureError::FunctionMetadataMismatch;
			return true;
		}

		if (m_Layout.ScriptFieldOffset
			> (std::numeric_limits<std::uintptr_t>::max)()
				- m_Request.Function.Function.Address)
		{
			m_Error = BlueprintBytecodeLiveCaptureError::HeaderAddressOverflow;
			return true;
		}
		const std::uintptr_t headerAddress =
			m_Request.Function.Function.Address + m_Layout.ScriptFieldOffset;
		std::uintptr_t headerEnd = 0;
		if (!CheckedAddressRange(
			headerAddress,
			m_Layout.HeaderByteWidth,
			headerEnd))
		{
			m_Error = BlueprintBytecodeLiveCaptureError::HeaderAddressOverflow;
			return true;
		}

		std::array<std::byte, BlueprintBytecodeCaptureSource::kMaxHeaderBytes>
			firstBytes{};
		std::array<std::byte, BlueprintBytecodeCaptureSource::kMaxHeaderBytes>
			finalBytes{};
		const std::span<std::byte> first = std::span(firstBytes).first(
			m_Layout.HeaderByteWidth);
		MemoryResult memory = ReadMemory(headerAddress, first);
		if (!memory.Ok())
		{
			m_Error = BlueprintBytecodeLiveCaptureError::HeaderReadFailed;
			m_MemoryError = memory.Error;
			return true;
		}

		ScriptArrayHeader header;
		if (!TryDecodeHeader(first, m_Layout, header)
			|| header.Num < 0
			|| header.Max < 0
			|| header.Max < header.Num)
		{
			m_Error = BlueprintBytecodeLiveCaptureError::HeaderInvalid;
			return true;
		}
		if (header.Num == 0)
		{
			m_Error = BlueprintBytecodeLiveCaptureError::EvidenceUnavailable;
			return true;
		}
		if (header.Num > static_cast<std::int32_t>(m_Request.MaxScriptBytes)
			|| header.Num > static_cast<std::int32_t>(
				BlueprintBytecodeEvidenceStore::kMaxScriptBytes)
			|| header.Max > BlueprintBytecodeEvidenceStore::kMaxScriptCapacity)
		{
			m_Error = BlueprintBytecodeLiveCaptureError::ScriptLimitExceeded;
			return true;
		}
		if (header.Data == 0)
		{
			m_Error = BlueprintBytecodeLiveCaptureError::ScriptAddressInvalid;
			return true;
		}
		std::uintptr_t scriptEnd = 0;
		if (!CheckedAddressRange(
			header.Data,
			static_cast<std::size_t>(header.Num),
			scriptEnd))
		{
			m_Error = BlueprintBytecodeLiveCaptureError::ScriptAddressInvalid;
			return true;
		}

		const std::span<std::uint8_t> script = std::span(m_Capture.Script).first(
			static_cast<std::size_t>(header.Num));
		memory = ReadMemory(header.Data, std::as_writable_bytes(script));
		if (!memory.Ok())
		{
			m_Error = BlueprintBytecodeLiveCaptureError::ScriptReadFailed;
			m_MemoryError = memory.Error;
			return true;
		}

		const std::span<std::byte> final = std::span(finalBytes).first(
			m_Layout.HeaderByteWidth);
		memory = ReadMemory(headerAddress, final);
		if (!memory.Ok())
		{
			m_Error = BlueprintBytecodeLiveCaptureError::HeaderReadFailed;
			m_MemoryError = memory.Error;
			return true;
		}
		ScriptArrayHeader finalHeader;
		if (!TryDecodeHeader(final, m_Layout, finalHeader))
		{
			m_Error = BlueprintBytecodeLiveCaptureError::HeaderInvalid;
			return true;
		}
		if (!std::equal(first.begin(), first.end(), final.begin(), final.end())
			|| finalHeader.Data != header.Data
			|| finalHeader.Num != header.Num
			|| finalHeader.Max != header.Max)
		{
			m_Error = BlueprintBytecodeLiveCaptureError::HeaderChangedDuringCopy;
			return true;
		}
		// Every supported UE4/UE5 Script bytecode stream is terminated by
		// EX_EndOfScript (0x53). Do not publish a plausible TArray from a
		// wrong/custom layout as bytecode evidence.
		if (script.empty() || script.back() != 0x53)
		{
			m_Error = BlueprintBytecodeLiveCaptureError::ScriptTerminatorInvalid;
			return true;
		}
		if (!ValidateFunctionIdentity())
			return true;
		if (!DependenciesCurrent() || !MetadataMatches())
		{
			m_Error = BlueprintBytecodeLiveCaptureError::DependencyChanged;
			return true;
		}

		m_Capture.Script.resize(static_cast<std::size_t>(header.Num));
		m_Capture.ScriptDataAddress = header.Data;
		m_Capture.ScriptNum = header.Num;
		m_Capture.ScriptMax = header.Max;
		m_Capture.CapturedAtMonotonicUs = MonotonicMicroseconds();
		m_Capture.HeaderWitnessFingerprint =
			ComputeBlueprintScriptHeaderFingerprint(m_Capture);
		m_Capture.EvidenceFingerprint =
			ComputeBlueprintBytecodeCaptureFingerprint(m_Capture);
		m_Error = BlueprintBytecodeLiveCaptureError::None;
		return true;
	}

	// A timed-out running task may outlive CaptureWithDiagnostics. Keep the
	// source barrier leased until the executor releases this owned work item.
	CallbackBarrier::Lease m_SourceLease;
	EngineFacade& m_Engine;
	std::shared_ptr<const EngineSnapshot> m_Objects;
	std::shared_ptr<const TypeSnapshot> m_Types;
	BlueprintScriptArrayLayoutWitness m_Layout;
	BlueprintBytecodeCaptureRequest m_Request;
	BlueprintBytecodeCapture m_Capture;
	BlueprintBytecodeLiveCaptureError m_Error =
		BlueprintBytecodeLiveCaptureError::EvidenceUnavailable;
	HandleError m_HandleError = HandleError::None;
	MemoryError m_MemoryError = MemoryError::None;
};

} // namespace

std::uint64_t ComputeBlueprintScriptArrayLayoutWitnessFingerprint(
	const BlueprintScriptArrayLayoutWitness& witness) noexcept
{
	std::uint64_t hash = kFnvOffset;
	AppendText(hash, "UExplorer.BlueprintScriptArrayLayoutWitness.v1");
	AppendBinding(hash, witness.Binding);
	AppendText(hash, witness.Source);
	AppendByte(hash, witness.Validated ? 1 : 0);
	AppendUnsigned(hash, witness.ScriptFieldOffset);
	AppendUnsigned(hash, witness.HeaderByteWidth);
	AppendUnsigned(hash, witness.DataPointerOffset);
	AppendByte(hash, witness.DataPointerWidth);
	AppendUnsigned(hash, witness.NumOffset);
	AppendByte(hash, witness.NumWidth);
	AppendUnsigned(hash, witness.MaxOffset);
	AppendByte(hash, witness.MaxWidth);
	AppendByte(hash, witness.ElementWidth);
	AppendByte(hash, witness.CountsSigned ? 1 : 0);
	AppendByte(hash, static_cast<std::uint8_t>(witness.ByteOrder));
	return FinishFingerprint(hash);
}

bool IsBlueprintScriptArrayLayoutWitnessValid(
	const BlueprintScriptArrayLayoutWitness& witness) noexcept
{
	return IsValidBinding(witness.Binding)
		&& IsBoundedText(
			witness.Source,
			BlueprintBytecodeEvidenceStore::kMaxSourceBytes)
		&& witness.Validated
		&& witness.ScriptFieldOffset > 0
		&& witness.ScriptFieldOffset
			<= BlueprintBytecodeEvidenceStore::kMaxScriptFieldOffset
		&& witness.HeaderByteWidth > 0
		&& witness.HeaderByteWidth
			<= BlueprintBytecodeCaptureSource::kMaxHeaderBytes
		&& (witness.DataPointerWidth == 4 || witness.DataPointerWidth == 8)
		&& IsIntegerWidth(witness.NumWidth)
		&& IsIntegerWidth(witness.MaxWidth)
		&& FieldFits(
			witness.DataPointerOffset,
			witness.DataPointerWidth,
			witness.HeaderByteWidth)
		&& FieldFits(witness.NumOffset, witness.NumWidth, witness.HeaderByteWidth)
		&& FieldFits(witness.MaxOffset, witness.MaxWidth, witness.HeaderByteWidth)
		&& !FieldsOverlap(
			witness.DataPointerOffset,
			witness.DataPointerWidth,
			witness.NumOffset,
			witness.NumWidth)
		&& !FieldsOverlap(
			witness.DataPointerOffset,
			witness.DataPointerWidth,
			witness.MaxOffset,
			witness.MaxWidth)
		&& !FieldsOverlap(
			witness.NumOffset,
			witness.NumWidth,
			witness.MaxOffset,
			witness.MaxWidth)
		&& witness.ElementWidth == 1
		&& witness.CountsSigned
		&& witness.ByteOrder == BlueprintScriptArrayByteOrder::LittleEndian
		&& witness.EvidenceFingerprint != 0
		&& witness.EvidenceFingerprint
			== ComputeBlueprintScriptArrayLayoutWitnessFingerprint(witness);
}

const char* ToString(const BlueprintBytecodeLiveCaptureError error) noexcept
{
	switch (error)
	{
	case BlueprintBytecodeLiveCaptureError::None: return "NONE";
	case BlueprintBytecodeLiveCaptureError::SourceInvalid: return "BLUEPRINT_CAPTURE_SOURCE_INVALID";
	case BlueprintBytecodeLiveCaptureError::SourceStopped: return "BLUEPRINT_CAPTURE_SOURCE_STOPPED";
	case BlueprintBytecodeLiveCaptureError::RequestInvalid: return "BLUEPRINT_CAPTURE_REQUEST_INVALID";
	case BlueprintBytecodeLiveCaptureError::EvidenceUnavailable: return "BLUEPRINT_BYTECODE_EVIDENCE_UNAVAILABLE";
	case BlueprintBytecodeLiveCaptureError::EvidenceStoreStopped: return "BLUEPRINT_EVIDENCE_STORE_STOPPED";
	case BlueprintBytecodeLiveCaptureError::LayoutUnavailable: return "BLUEPRINT_SCRIPT_LAYOUT_UNAVAILABLE";
	case BlueprintBytecodeLiveCaptureError::LayoutInvalid: return "BLUEPRINT_SCRIPT_LAYOUT_INVALID";
	case BlueprintBytecodeLiveCaptureError::BindingMismatch: return "BLUEPRINT_CAPTURE_BINDING_MISMATCH";
	case BlueprintBytecodeLiveCaptureError::DependencyUnavailable: return "BLUEPRINT_CAPTURE_DEPENDENCY_UNAVAILABLE";
	case BlueprintBytecodeLiveCaptureError::DependencyChanged: return "BLUEPRINT_CAPTURE_DEPENDENCY_CHANGED";
	case BlueprintBytecodeLiveCaptureError::ExecutionThreadInvalid: return "BLUEPRINT_CAPTURE_EXECUTION_THREAD_INVALID";
	case BlueprintBytecodeLiveCaptureError::FunctionHandleStale: return "BLUEPRINT_CAPTURE_FUNCTION_HANDLE_STALE";
	case BlueprintBytecodeLiveCaptureError::FunctionMetadataMismatch: return "BLUEPRINT_CAPTURE_FUNCTION_METADATA_MISMATCH";
	case BlueprintBytecodeLiveCaptureError::HeaderAddressOverflow: return "BLUEPRINT_SCRIPT_HEADER_ADDRESS_OVERFLOW";
	case BlueprintBytecodeLiveCaptureError::HeaderReadFailed: return "BLUEPRINT_SCRIPT_HEADER_READ_FAILED";
	case BlueprintBytecodeLiveCaptureError::HeaderInvalid: return "BLUEPRINT_SCRIPT_HEADER_INVALID";
	case BlueprintBytecodeLiveCaptureError::ScriptAddressInvalid: return "BLUEPRINT_SCRIPT_ADDRESS_INVALID";
	case BlueprintBytecodeLiveCaptureError::ScriptLimitExceeded: return "BLUEPRINT_SCRIPT_LIMIT_EXCEEDED";
	case BlueprintBytecodeLiveCaptureError::ScriptReadFailed: return "BLUEPRINT_SCRIPT_READ_FAILED";
	case BlueprintBytecodeLiveCaptureError::ScriptTerminatorInvalid: return "BLUEPRINT_SCRIPT_TERMINATOR_INVALID";
	case BlueprintBytecodeLiveCaptureError::HeaderChangedDuringCopy: return "BLUEPRINT_SCRIPT_HEADER_CHANGED_DURING_COPY";
	case BlueprintBytecodeLiveCaptureError::ExecutorDisabled: return "BLUEPRINT_CAPTURE_EXECUTOR_DISABLED";
	case BlueprintBytecodeLiveCaptureError::ExecutorCancelled: return "BLUEPRINT_CAPTURE_EXECUTOR_CANCELLED";
	case BlueprintBytecodeLiveCaptureError::ExecutorQueueBusy: return "BLUEPRINT_CAPTURE_EXECUTOR_QUEUE_BUSY";
	case BlueprintBytecodeLiveCaptureError::ExecutorWaitDenied: return "BLUEPRINT_CAPTURE_EXECUTOR_WAIT_DENIED";
	case BlueprintBytecodeLiveCaptureError::ExecutorTimedOut: return "BLUEPRINT_CAPTURE_EXECUTOR_TIMED_OUT";
	case BlueprintBytecodeLiveCaptureError::ExecutorFailed: return "BLUEPRINT_CAPTURE_EXECUTOR_FAILED";
	case BlueprintBytecodeLiveCaptureError::PublishFailed: return "BLUEPRINT_CAPTURE_PUBLISH_FAILED";
	case BlueprintBytecodeLiveCaptureError::AllocationFailed: return "BLUEPRINT_CAPTURE_ALLOCATION_FAILED";
	case BlueprintBytecodeLiveCaptureError::InternalError: return "BLUEPRINT_CAPTURE_INTERNAL_ERROR";
	}
	return "BLUEPRINT_CAPTURE_UNKNOWN_ERROR";
}

BlueprintBytecodeCaptureSource::BlueprintBytecodeCaptureSource(
	EngineFacade& engine,
	GameThreadExecutor& gameThread,
	BlueprintBytecodeEvidenceStore& evidenceStore,
	BlueprintScriptArrayLayoutWitness layout,
	const int timeoutMs)
	: m_Engine(engine),
	  m_GameThread(gameThread),
	  m_EvidenceStore(evidenceStore),
	  m_Layout(std::move(layout)),
	  m_TimeoutMs(timeoutMs)
{
}

bool BlueprintBytecodeCaptureSource::IsConfigured() const noexcept
{
	return m_Engine.IsConfigured()
		&& m_Engine.SessionId() == m_Layout.Binding.SessionId
		&& m_Engine.ContextGeneration() == m_Layout.Binding.ContextGeneration
		&& m_EvidenceStore.IsConfigured()
		&& !m_EvidenceStore.IsStopped()
		&& SameBlueprintEvidenceBinding(
			m_EvidenceStore.Binding(),
			m_Layout.Binding)
		&& IsBlueprintScriptArrayLayoutWitnessValid(m_Layout)
		&& m_TimeoutMs > 0
		&& m_TimeoutMs <= GameThreadExecutor::kMaxTimeoutMs;
}

BlueprintBytecodeCaptureResult BlueprintBytecodeCaptureSource::Capture(
	const BlueprintBytecodeCaptureRequest& request)
{
	BlueprintBytecodeLiveCaptureResult result = CaptureWithDiagnostics(request);
	return {
		.Error = result.SourceError,
		.Capture = std::move(result.Capture)
	};
}

BlueprintBytecodeLiveCaptureResult
BlueprintBytecodeCaptureSource::CaptureWithDiagnostics(
	const BlueprintBytecodeCaptureRequest& request) noexcept
{
	try
	{
		auto barrierLease = m_Barrier.Enter();
		if (!barrierLease.OwnedWorkAllowed())
		{
			return {
				.Error = BlueprintBytecodeLiveCaptureError::SourceStopped,
				.SourceError = BlueprintEvidenceSourceError::Stopped
			};
		}
		if (!IsValidRequest(request))
		{
			return {
				.Error = BlueprintBytecodeLiveCaptureError::RequestInvalid,
				.SourceError = BlueprintEvidenceSourceError::InvalidRequest
			};
		}
		if (!SameBlueprintEvidenceBinding(request.Binding, m_Layout.Binding)
			|| !SameBlueprintEvidenceBinding(
				request.Binding,
				m_EvidenceStore.Binding()))
		{
			return {
				.Error = BlueprintBytecodeLiveCaptureError::BindingMismatch,
				.SourceError = BlueprintEvidenceSourceError::DependencyChanged
			};
		}

		BlueprintBytecodeCaptureResult existing = m_EvidenceStore.Capture(request);
		if (existing.Ok())
		{
			return {
				.Error = BlueprintBytecodeLiveCaptureError::None,
				.SourceError = BlueprintEvidenceSourceError::None,
				.SubmitResult = GameThreadSubmitResult::Completed,
				.Capture = std::move(existing.Capture)
			};
		}
		if (existing.Error != BlueprintEvidenceSourceError::Unavailable)
		{
			const BlueprintBytecodeLiveCaptureError error =
				MapLookupError(existing.Error);
			return {.Error = error, .SourceError = existing.Error};
		}

		if (!m_Layout.Validated || m_Layout.EvidenceFingerprint == 0)
		{
			return {
				.Error = BlueprintBytecodeLiveCaptureError::LayoutUnavailable,
				.SourceError = BlueprintEvidenceSourceError::Unavailable
			};
		}
		if (!IsBlueprintScriptArrayLayoutWitnessValid(m_Layout))
		{
			return {
				.Error = BlueprintBytecodeLiveCaptureError::LayoutInvalid,
				.SourceError = BlueprintEvidenceSourceError::InvalidEvidence
			};
		}
		if (!IsConfigured())
		{
			const BlueprintBytecodeLiveCaptureError error =
				m_EvidenceStore.IsStopped()
					? BlueprintBytecodeLiveCaptureError::EvidenceStoreStopped
					: BlueprintBytecodeLiveCaptureError::SourceInvalid;
			return {.Error = error, .SourceError = MapSourceError(error)};
		}

		std::shared_ptr<const EngineSnapshot> objects =
			m_Engine.Snapshots().Current();
		std::shared_ptr<const TypeSnapshot> types =
			m_Engine.Types().Current();
		if (!objects || !types)
		{
			return {
				.Error = BlueprintBytecodeLiveCaptureError::DependencyUnavailable,
				.SourceError = BlueprintEvidenceSourceError::Unavailable
			};
		}
		if (objects->SessionId != request.Binding.SessionId
			|| objects->ContextGeneration != request.Binding.ContextGeneration
			|| objects->Generation != request.Binding.ObjectSnapshotGeneration
			|| types->SessionId() != request.Binding.SessionId
			|| types->ContextGeneration() != request.Binding.ContextGeneration
			|| types->ObjectSnapshotGeneration()
				!= request.Binding.ObjectSnapshotGeneration
			|| types->Generation() != request.Binding.TypeSnapshotGeneration)
		{
			return {
				.Error = BlueprintBytecodeLiveCaptureError::DependencyChanged,
				.SourceError = BlueprintEvidenceSourceError::DependencyChanged
			};
		}

		auto work = std::make_shared<BlueprintBytecodeCaptureWork>(
			m_Engine,
			std::move(objects),
			std::move(types),
			m_Layout,
			request,
			std::move(barrierLease));
		const GameThreadSubmitResult submitted =
			m_GameThread.SubmitOwned(work, m_TimeoutMs);
		if (submitted != GameThreadSubmitResult::Completed)
		{
			const BlueprintBytecodeLiveCaptureError error =
				MapSubmitResult(submitted);
			return {
				.Error = error,
				.SourceError = MapSourceError(error),
				.SubmitResult = submitted
			};
		}
		if (work->Error() != BlueprintBytecodeLiveCaptureError::None)
		{
			return {
				.Error = work->Error(),
				.SourceError = MapSourceError(work->Error()),
				.HandleValidationError = work->HandleValidationError(),
				.MemoryReadError = work->MemoryReadError(),
				.SubmitResult = submitted
			};
		}

		const BlueprintEvidencePublishError published =
			m_EvidenceStore.PublishCapture(work->TakeCapture());
		if (published != BlueprintEvidencePublishError::None
			&& published != BlueprintEvidencePublishError::DuplicateConflict)
		{
			BlueprintBytecodeLiveCaptureError error =
				BlueprintBytecodeLiveCaptureError::PublishFailed;
			if (published == BlueprintEvidencePublishError::StoreStopped)
				error = BlueprintBytecodeLiveCaptureError::EvidenceStoreStopped;
			else if (published == BlueprintEvidencePublishError::BindingMismatch)
				error = BlueprintBytecodeLiveCaptureError::BindingMismatch;
			else if (published == BlueprintEvidencePublishError::AllocationFailed)
				error = BlueprintBytecodeLiveCaptureError::AllocationFailed;
			return {
				.Error = error,
				.SourceError = MapSourceError(error),
				.PublishError = published,
				.SubmitResult = submitted
			};
		}

		BlueprintBytecodeCaptureResult captured = m_EvidenceStore.Capture(request);
		if (!captured.Ok())
		{
			const BlueprintBytecodeLiveCaptureError error =
				MapLookupError(captured.Error);
			return {
				.Error = error,
				.SourceError = captured.Error,
				.PublishError = published,
				.SubmitResult = submitted
			};
		}
		return {
			.Error = BlueprintBytecodeLiveCaptureError::None,
			.SourceError = BlueprintEvidenceSourceError::None,
			.PublishError = published,
			.SubmitResult = submitted,
			.Capture = std::move(captured.Capture)
		};
	}
	catch (const std::bad_alloc&)
	{
		return {
			.Error = BlueprintBytecodeLiveCaptureError::AllocationFailed,
			.SourceError = BlueprintEvidenceSourceError::AllocationFailed
		};
	}
	catch (...)
	{
		return {
			.Error = BlueprintBytecodeLiveCaptureError::InternalError,
			.SourceError = BlueprintEvidenceSourceError::InternalError
		};
	}
}

bool BlueprintBytecodeCaptureSource::StopAndDrain(
	const std::chrono::milliseconds timeout)
{
	m_Barrier.BeginStopping();
	return m_Barrier.WaitForDrain(timeout);
}

} // namespace UExplorer::Runtime
