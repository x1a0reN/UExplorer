#include "ObjectArrayIdentitySource.h"

#include "GameThreadExecutor.h"
#include "SafeMemory.h"
#include "Unreal/Enums.h"
#include "Unreal/ObjectArray.h"

#include <Windows.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace UExplorer::Runtime
{
namespace
{

constexpr std::size_t kMaxOuterDepth = 128;
constexpr std::size_t kMaxCanonicalPathLength = 4096;
constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

std::uint64_t HashBytes(
	std::uint64_t hash,
	const void* data,
	const std::size_t size) noexcept
{
	const auto* bytes = static_cast<const std::uint8_t*>(data);
	for (std::size_t index = 0; index < size; ++index)
	{
		hash ^= bytes[index];
		hash *= kFnvPrime;
	}
	return hash;
}

template<typename T>
std::uint64_t HashValue(std::uint64_t hash, const T& value) noexcept
{
	return HashBytes(hash, &value, sizeof(value));
}

bool TryAddFieldAddress(
	const std::uintptr_t base,
	const std::int32_t offset,
	const std::size_t size,
	std::uintptr_t& address) noexcept
{
	address = 0;
	if (base == 0 || offset <= 0)
		return false;
	const auto unsignedOffset = static_cast<std::uintptr_t>(offset);
	if (unsignedOffset > (std::numeric_limits<std::uintptr_t>::max)() - base)
		return false;
	address = base + unsignedOffset;
	std::uintptr_t ignored = 0;
	return CheckedAddressRange(address, size, ignored);
}

bool AppendUnsigned(
	std::string& output,
	const std::uint64_t value,
	const int base)
{
	std::array<char, 32> buffer{};
	const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, base);
	if (result.ec != std::errc{})
		return false;
	output.append(buffer.data(), result.ptr);
	return true;
}

bool TryReadCanonicalFNameToken(
	const ObjectIdentityContext& offsets,
	const std::uintptr_t objectAddress,
	std::string& output)
{
	if (!offsets.CanIssueFunctionHandles())
	{
		return false;
	}

	std::uintptr_t nameBase = 0;
	if (!TryAddFieldAddress(
		objectAddress,
		offsets.ObjectName,
		static_cast<std::size_t>(offsets.FNameSize),
		nameBase))
	{
		return false;
	}
	const auto comparisonOffset = static_cast<std::uint32_t>(offsets.FNameComparisonIndex);
	if (comparisonOffset + sizeof(std::uint32_t)
		> static_cast<std::uint32_t>(offsets.FNameSize))
	{
		return false;
	}
	std::uint32_t comparisonIndex = 0;
	if (!ReadValue(nameBase + comparisonOffset, comparisonIndex).Ok())
		return false;

	std::uint32_t number = 0;
	if (offsets.FNameNumber >= 0)
	{
		const auto numberOffset = static_cast<std::uint32_t>(offsets.FNameNumber);
		if (numberOffset + sizeof(std::uint32_t)
			> static_cast<std::uint32_t>(offsets.FNameSize)
			|| !ReadValue(nameBase + numberOffset, number).Ok())
		{
			return false;
		}
	}

	output = "fname:";
	if (!AppendUnsigned(output, comparisonIndex, 16))
		return false;
	output.push_back(':');
	return AppendUnsigned(output, number, 10);
}

bool TryBuildCanonicalFunctionPath(
	const ObjectIdentityContext& offsets,
	const std::uintptr_t functionAddress,
	std::string& output)
{
	std::array<std::uintptr_t, kMaxOuterDepth> chain{};
	std::size_t depth = 0;
	std::uintptr_t current = functionAddress;
	while (current != 0)
	{
		if (depth == chain.size())
			return false;
		for (std::size_t index = 0; index < depth; ++index)
		{
			if (chain[index] == current)
				return false;
		}
		chain[depth++] = current;

		std::uintptr_t outerField = 0;
		if (!TryAddFieldAddress(current, offsets.ObjectOuter, sizeof(void*), outerField))
			return false;
		void* outer = nullptr;
		if (!ReadValue(outerField, outer).Ok())
			return false;
		current = reinterpret_cast<std::uintptr_t>(outer);
	}

	output = "Function ";
	for (std::size_t reverseIndex = depth; reverseIndex > 0; --reverseIndex)
	{
		std::string nameToken;
		if (!TryReadCanonicalFNameToken(offsets, chain[reverseIndex - 1], nameToken))
			return false;
		if (output.size() + nameToken.size() + 1 > kMaxCanonicalPathLength)
			return false;
		if (reverseIndex != depth)
			output.push_back('.');
		output += nameToken;
	}
	return depth > 1;
}

bool SameIdentity(const ObjectIdentity& left, const ObjectIdentity& right) noexcept
{
	return left.Index == right.Index
		&& left.SerialNumber == right.SerialNumber
		&& left.Address == right.Address
		&& left.ClassFingerprint == right.ClassFingerprint;
}

} // namespace

ObjectArrayIdentitySource::ObjectArrayIdentitySource(
	std::shared_ptr<const EngineContext> context)
	: m_Context(std::move(context)),
	  m_Offsets(m_Context ? CaptureObjectIdentityContext(*m_Context) : ObjectIdentityContext{})
{
}

bool ObjectArrayIdentitySource::CanIssueObjectHandles() const noexcept
{
	const FUObjectItemIdentityLayout& layout = ObjectArray::GetIdentityLayout();
	return m_Context
		&& m_Offsets.CanIssueObjectHandles()
		&& layout.Validated
		&& layout.SerialOffset >= 0
		&& layout.SerialOffset == m_Offsets.FUObjectItemSerial;
}

bool ObjectArrayIdentitySource::CanIssueFunctionHandles() const noexcept
{
	return CanIssueObjectHandles() && m_Offsets.CanIssueFunctionHandles();
}

bool ObjectArrayIdentitySource::IsCurrentExecutionThreadValid() const noexcept
{
	const GameThreadExecutor& executor = GetGameThreadExecutor();
	const GameThreadDiagnostics diagnostics = executor.GetDiagnostics();
	return diagnostics.Enabled
		&& diagnostics.PumpObserved
		&& diagnostics.PumpThreadStable
		&& executor.IsCurrentPumpThread();
}

bool ObjectArrayIdentitySource::TryReadObjectCore(
	const std::int32_t index,
	ObjectIdentity& identity) const
{
	identity = {};
	FUObjectItemIdentity item;
	if (!ObjectArray::TryReadIdentity(index, item))
		return false;

	std::uintptr_t classField = 0;
	if (!TryAddFieldAddress(item.ObjectAddress, m_Offsets.ObjectClass, sizeof(void*), classField))
		return false;
	void* classObject = nullptr;
	if (!ReadValue(classField, classObject).Ok() || !classObject)
		return false;

	std::uintptr_t classIndexField = 0;
	if (!TryAddFieldAddress(
		reinterpret_cast<std::uintptr_t>(classObject),
		m_Offsets.ObjectIndex,
		sizeof(std::int32_t),
		classIndexField))
	{
		return false;
	}
	std::int32_t classIndex = -1;
	if (!ReadValue(classIndexField, classIndex).Ok() || classIndex < 0)
		return false;
	FUObjectItemIdentity classItem;
	if (!ObjectArray::TryReadIdentity(classIndex, classItem)
		|| classItem.ObjectAddress != reinterpret_cast<std::uintptr_t>(classObject))
	{
		return false;
	}

	// A live object keeps its UClass alive. Do not hash the class serial because
	// UE may lazily transition that serial from zero to positive without recycling it.
	std::uint64_t classFingerprint = kFnvOffsetBasis;
	classFingerprint = HashValue(classFingerprint, classItem.ObjectAddress);
	classFingerprint = HashValue(classFingerprint, classItem.Index);
	if (classFingerprint == 0)
		classFingerprint = 1;

	FUObjectItemIdentity finalItem;
	if (!ObjectArray::TryReadIdentity(index, finalItem)
		|| finalItem.Index != item.Index
		|| finalItem.SerialNumber != item.SerialNumber
		|| finalItem.ObjectAddress != item.ObjectAddress)
	{
		return false;
	}

	identity = {
		.Index = item.Index,
		.SerialNumber = item.SerialNumber,
		.Address = item.ObjectAddress,
		.ClassFingerprint = classFingerprint
	};
	return true;
}

bool ObjectArrayIdentitySource::TryReadObject(
	const std::int32_t index,
	ObjectIdentity& identity)
{
	if (!CanIssueObjectHandles() || !IsCurrentExecutionThreadValid())
		return false;
	return TryReadObjectCore(index, identity);
}

bool ObjectArrayIdentitySource::TryReadFunction(
	const std::int32_t index,
	FunctionIdentity& identity)
{
	identity = {};
	if (!CanIssueFunctionHandles() || !IsCurrentExecutionThreadValid())
	{
		return false;
	}

	ObjectIdentity functionIdentity;
	if (!TryReadObjectCore(index, functionIdentity))
		return false;

	std::uintptr_t classField = 0;
	if (!TryAddFieldAddress(functionIdentity.Address, m_Offsets.ObjectClass, sizeof(void*), classField))
		return false;
	void* classObject = nullptr;
	if (!ReadValue(classField, classObject).Ok() || !classObject)
		return false;
	std::uintptr_t castFlagsField = 0;
	if (!TryAddFieldAddress(
		reinterpret_cast<std::uintptr_t>(classObject),
		m_Offsets.ClassCastFlags,
		sizeof(std::uint64_t),
		castFlagsField))
	{
		return false;
	}
	std::uint64_t castFlags = 0;
	if (!ReadValue(castFlagsField, castFlags).Ok()
		|| (castFlags & static_cast<std::uint64_t>(EClassCastFlags::Function)) == 0)
	{
		return false;
	}

	std::uintptr_t outerField = 0;
	if (!TryAddFieldAddress(functionIdentity.Address, m_Offsets.ObjectOuter, sizeof(void*), outerField))
		return false;
	void* ownerObject = nullptr;
	if (!ReadValue(outerField, ownerObject).Ok() || !ownerObject)
		return false;
	std::uintptr_t ownerIndexField = 0;
	if (!TryAddFieldAddress(
		reinterpret_cast<std::uintptr_t>(ownerObject),
		m_Offsets.ObjectIndex,
		sizeof(std::int32_t),
		ownerIndexField))
	{
		return false;
	}
	std::int32_t ownerIndex = -1;
	if (!ReadValue(ownerIndexField, ownerIndex).Ok() || ownerIndex < 0)
		return false;
	ObjectIdentity ownerIdentity;
	if (!TryReadObjectCore(ownerIndex, ownerIdentity)
		|| ownerIdentity.Address != reinterpret_cast<std::uintptr_t>(ownerObject))
	{
		return false;
	}

	std::string fullPath;
	if (!TryBuildCanonicalFunctionPath(m_Offsets, functionIdentity.Address, fullPath))
		return false;

	std::uintptr_t flagsField = 0;
	std::uintptr_t sizeField = 0;
	if (!TryAddFieldAddress(
		functionIdentity.Address,
		m_Offsets.FunctionFlags,
		sizeof(std::uint32_t),
		flagsField)
		|| !TryAddFieldAddress(
			functionIdentity.Address,
			m_Offsets.StructSize,
			sizeof(std::int32_t),
			sizeField))
	{
		return false;
	}
	std::uint32_t functionFlags = 0;
	std::int32_t parameterStructSize = 0;
	if (!ReadValue(flagsField, functionFlags).Ok()
		|| !ReadValue(sizeField, parameterStructSize).Ok()
		|| parameterStructSize < 0
		|| parameterStructSize > 1024 * 1024)
	{
		return false;
	}

	std::uintptr_t execFunction = 0;
	if (m_Offsets.FunctionExec > 0)
	{
		std::uintptr_t execField = 0;
		if (!TryAddFieldAddress(
			functionIdentity.Address,
			m_Offsets.FunctionExec,
			sizeof(void*),
			execField)
			|| !ReadValue(execField, execFunction).Ok())
		{
			return false;
		}
	}

	std::uint64_t signature = HashBytes(
		kFnvOffsetBasis,
		fullPath.data(),
		fullPath.size());
	signature = HashValue(signature, functionFlags);
	signature = HashValue(signature, parameterStructSize);
	signature = HashValue(signature, execFunction);
	signature = HashValue(signature, ownerIdentity.Index);
	signature = HashValue(signature, ownerIdentity.SerialNumber);
	signature = HashValue(signature, ownerIdentity.Address);
	if (signature == 0)
		signature = 1;

	ObjectIdentity functionFinal;
	ObjectIdentity ownerFinal;
	std::string finalPath;
	void* finalOwnerObject = nullptr;
	std::uint32_t finalFunctionFlags = 0;
	std::int32_t finalParameterStructSize = 0;
	std::uintptr_t finalExecFunction = 0;
	if (!TryReadObjectCore(index, functionFinal)
		|| !TryReadObjectCore(ownerIndex, ownerFinal)
		|| !SameIdentity(functionIdentity, functionFinal)
		|| !SameIdentity(ownerIdentity, ownerFinal)
		|| !TryBuildCanonicalFunctionPath(m_Offsets, functionIdentity.Address, finalPath)
		|| finalPath != fullPath
		|| !ReadValue(outerField, finalOwnerObject).Ok()
		|| finalOwnerObject != ownerObject
		|| !ReadValue(flagsField, finalFunctionFlags).Ok()
		|| finalFunctionFlags != functionFlags
		|| !ReadValue(sizeField, finalParameterStructSize).Ok()
		|| finalParameterStructSize != parameterStructSize)
	{
		return false;
	}
	if (m_Offsets.FunctionExec > 0)
	{
		std::uintptr_t execField = 0;
		if (!TryAddFieldAddress(
			functionIdentity.Address,
			m_Offsets.FunctionExec,
			sizeof(void*),
			execField)
			|| !ReadValue(execField, finalExecFunction).Ok()
			|| finalExecFunction != execFunction)
		{
			return false;
		}
	}

	identity = {
		.Function = functionIdentity,
		.Owner = ownerIdentity,
		.FullPath = std::move(fullPath),
		.SignatureFingerprint = signature
	};
	return true;
}

} // namespace UExplorer::Runtime
