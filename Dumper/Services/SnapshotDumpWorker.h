#pragma once

#include "Runtime/DumpJobCoordinator.h"
#include "Runtime/EngineContext.h"
#include "Runtime/EngineSnapshot.h"
#include "Runtime/TypeSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace UExplorer::Services
{

enum class SnapshotDumpFormat : std::uint8_t
{
	Sdk,
	Usmap,
	Dumpspace,
	IdaScript
};

const char* ToString(SnapshotDumpFormat format) noexcept;
std::optional<SnapshotDumpFormat> ParseSnapshotDumpFormat(
	std::string_view value) noexcept;

// This binding is immutable and pins the exact generations for the complete
// worker callback. It is removed from retained job summaries at finalization.
struct SnapshotDumpInput final : Runtime::IDumpJobInput
{
	SnapshotDumpFormat Format = SnapshotDumpFormat::Sdk;
	std::shared_ptr<const Runtime::EngineContext> Context;
	std::shared_ptr<const Runtime::EngineSnapshot> Objects;
	std::shared_ptr<const Runtime::TypeSnapshot> Types;
};

struct SnapshotDumpWorkerLimits
{
	std::size_t MaxTypes = 250'000;
	std::size_t MaxMembers = 4'000'000;
	std::size_t MaxArtifacts = 8;
	std::size_t MaxArtifactBytes = 128 * 1024 * 1024;
	std::size_t MaxTotalArtifactBytes = 256 * 1024 * 1024;
	std::size_t MaxOutputIdentityBytes = 128;
};

// Generates only from immutable snapshot state. It never reads live UE memory,
// legacy Off/Settings globals, or generator wrapper singletons.
class SnapshotDumpWorker final : public Runtime::IDumpJobWorker
{
public:
	static constexpr std::size_t kHardMaxTypes = 1'000'000;
	static constexpr std::size_t kHardMaxMembers = 8'000'000;
	static constexpr std::size_t kHardMaxArtifacts = 32;
	static constexpr std::size_t kHardMaxArtifactBytes = 512 * 1024 * 1024;
	static constexpr std::size_t kHardMaxTotalArtifactBytes = 1024ULL * 1024ULL * 1024ULL;

	SnapshotDumpWorker(
		std::filesystem::path outputRoot,
		SnapshotDumpWorkerLimits limits = {});

	bool IsConfigured() const noexcept { return m_Configured; }
	const std::filesystem::path& OutputRoot() const noexcept { return m_OutputRoot; }

	Runtime::DumpJobWorkerResult Execute(
		const std::shared_ptr<const Runtime::DumpJobRequest>& request,
		Runtime::IDumpJobExecutionContext& context) override;

private:
	std::filesystem::path m_OutputRoot;
	SnapshotDumpWorkerLimits m_Limits;
	bool m_Configured = false;
};

// Resolves %LOCALAPPDATA%/UExplorer/Dumps without consulting project settings.
// Failure is explicit; callers must not substitute the current directory.
std::optional<std::filesystem::path> ResolveDefaultSnapshotDumpRoot() noexcept;

} // namespace UExplorer::Services
