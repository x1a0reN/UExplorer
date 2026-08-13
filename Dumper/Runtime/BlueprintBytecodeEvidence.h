#pragma once

#include "Blueprint/BlueprintDecompiler.h"
#include "ObjectHandle.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace UExplorer::Runtime
{

struct BlueprintEvidenceBinding
{
	std::string SessionId;
	std::uint64_t ContextGeneration = 0;
	std::uint64_t ObjectSnapshotGeneration = 0;
	std::uint64_t TypeSnapshotGeneration = 0;
};

bool SameBlueprintEvidenceBinding(
	const BlueprintEvidenceBinding& left,
	const BlueprintEvidenceBinding& right) noexcept;

struct BlueprintBytecodeCapture
{
	BlueprintEvidenceBinding Binding;
	FunctionHandle Function;
	std::string FunctionPath;
	std::string Source;
	std::uint32_t ScriptFieldOffset = 0;
	std::uintptr_t ScriptDataAddress = 0;
	std::int32_t ScriptNum = 0;
	std::int32_t ScriptMax = 0;
	// Computed only after identical pre/post TArray header witnesses bracket the copy.
	std::uint64_t HeaderWitnessFingerprint = 0;
	std::vector<std::uint8_t> Script;
	std::uint64_t CapturedAtMonotonicUs = 0;
	std::uint64_t EvidenceFingerprint = 0;
};

struct BlueprintBytecodeProfileRecord
{
	BlueprintEvidenceBinding Binding;
	std::string Source;
	BlueprintDecompiler::BytecodeProfile Definition;
	std::uint64_t EvidenceFingerprint = 0;
};

std::uint64_t ComputeBlueprintBytecodeCaptureFingerprint(
	const BlueprintBytecodeCapture& capture) noexcept;
std::uint64_t ComputeBlueprintScriptHeaderFingerprint(
	const BlueprintBytecodeCapture& capture) noexcept;
std::uint64_t ComputeBlueprintBytecodeProfileFingerprint(
	const BlueprintBytecodeProfileRecord& profile) noexcept;

struct BlueprintBytecodeCaptureRequest
{
	BlueprintEvidenceBinding Binding;
	FunctionHandle Function;
	std::string FunctionPath;
	std::size_t MaxScriptBytes = 0;
};

struct BlueprintBytecodeProfileRequest
{
	BlueprintEvidenceBinding Binding;
	std::string ProfileId;
};

enum class BlueprintEvidenceSourceError : std::uint8_t
{
	None,
	Unavailable,
	Stopped,
	InvalidRequest,
	DependencyChanged,
	FunctionHandleStale,
	ExecutionThreadInvalid,
	LimitExceeded,
	InvalidEvidence,
	AllocationFailed,
	InternalError
};

const char* ToString(BlueprintEvidenceSourceError error) noexcept;

struct BlueprintBytecodeCaptureResult
{
	BlueprintEvidenceSourceError Error = BlueprintEvidenceSourceError::Unavailable;
	std::shared_ptr<const BlueprintBytecodeCapture> Capture;

	bool Ok() const noexcept
	{
		return Error == BlueprintEvidenceSourceError::None
			&& static_cast<bool>(Capture);
	}
};

struct BlueprintBytecodeProfileResult
{
	BlueprintEvidenceSourceError Error = BlueprintEvidenceSourceError::Unavailable;
	std::shared_ptr<const BlueprintBytecodeProfileRecord> Profile;

	bool Ok() const noexcept
	{
		return Error == BlueprintEvidenceSourceError::None
			&& static_cast<bool>(Profile);
	}
};

// Implementations own all game-thread scheduling and must return an immutable
// byte copy. The command layer never dereferences a live UObject or borrowed TArray.
class IBlueprintBytecodeCaptureSource
{
public:
	virtual ~IBlueprintBytecodeCaptureSource() = default;
	virtual BlueprintBytecodeCaptureResult Capture(
		const BlueprintBytecodeCaptureRequest& request) = 0;
};

class IBlueprintBytecodeProfileSource
{
public:
	virtual ~IBlueprintBytecodeProfileSource() = default;
	virtual BlueprintBytecodeProfileResult ResolveProfile(
		const BlueprintBytecodeProfileRequest& request) const = 0;
};

enum class BlueprintEvidencePublishError : std::uint8_t
{
	None,
	StoreInvalid,
	StoreStopped,
	BindingMismatch,
	RecordInvalid,
	FingerprintMismatch,
	DuplicateConflict,
	AllocationFailed
};

const char* ToString(BlueprintEvidencePublishError error) noexcept;

// This store is generation-scoped. A record cannot be replaced within the
// generation; publishers must advance the owning snapshots instead.
class BlueprintBytecodeEvidenceStore final
	: public IBlueprintBytecodeCaptureSource,
	  public IBlueprintBytecodeProfileSource
{
public:
	static constexpr std::size_t kMaxSessionBytes = 128;
	static constexpr std::size_t kMaxPathBytes = 4096;
	static constexpr std::size_t kMaxSourceBytes = 1024;
	static constexpr std::size_t kMaxProfileIdBytes = 256;
	static constexpr std::size_t kMaxScriptBytes = 1024 * 1024;
	static constexpr std::int32_t kMaxScriptCapacity = 4 * 1024 * 1024;
	static constexpr std::uint32_t kMaxScriptFieldOffset = 1024 * 1024;
	static constexpr std::size_t kMaxInstructions = 100000;
	static constexpr std::size_t kMaxStringCodeUnits = 65536;
	static constexpr std::uint32_t kMaxRecursionDepth = 64;

	explicit BlueprintBytecodeEvidenceStore(BlueprintEvidenceBinding binding);
	BlueprintBytecodeEvidenceStore(const BlueprintBytecodeEvidenceStore&) = delete;
	BlueprintBytecodeEvidenceStore& operator=(const BlueprintBytecodeEvidenceStore&) = delete;

	bool IsConfigured() const noexcept;
	bool IsStopped() const noexcept
	{
		return m_Stopped.load(std::memory_order_acquire);
	}
	const BlueprintEvidenceBinding& Binding() const noexcept { return m_Binding; }

	BlueprintEvidencePublishError PublishCapture(
		BlueprintBytecodeCapture capture) noexcept;
	BlueprintEvidencePublishError PublishProfile(
		BlueprintBytecodeProfileRecord profile) noexcept;

	BlueprintBytecodeCaptureResult Capture(
		const BlueprintBytecodeCaptureRequest& request) override;
	BlueprintBytecodeProfileResult ResolveProfile(
		const BlueprintBytecodeProfileRequest& request) const override;

	void Stop() noexcept;

private:
	BlueprintEvidenceBinding m_Binding;
	mutable std::mutex m_Mutex;
	std::map<std::string, std::shared_ptr<const BlueprintBytecodeCapture>, std::less<>>
		m_Captures;
	std::map<std::string, std::shared_ptr<const BlueprintBytecodeProfileRecord>, std::less<>>
		m_Profiles;
	std::atomic<bool> m_Stopped{false};
};

} // namespace UExplorer::Runtime
