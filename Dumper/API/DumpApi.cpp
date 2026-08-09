#include "DumpApi.h"
#include "ApiCommon.h"

#include "Generators/Generator.h"
#include "Generators/CppGenerator.h"
#include "Generators/MappingGenerator.h"
#include "Generators/DumpspaceGenerator.h"
#include "Generators/IDAMappingGenerator.h"

#include "Settings.h"

#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <unordered_map>
#include <chrono>

namespace UExplorer::API
{

enum class JobStatus { Running, Completed, Failed };

struct DumpJob
{
	std::string Id;
	std::string Format;
	JobStatus Status = JobStatus::Running;
	std::string Error;
	std::string OutputPath;
	int64_t StartTime = 0;
	int64_t EndTime = 0;
};

static std::mutex g_JobsMutex;
static std::unordered_map<std::string, DumpJob> g_Jobs;
static std::atomic<int> g_JobCounter{ 0 };

static std::mutex g_ThreadsMutex;
static std::condition_variable g_DumpStoppedCV;
static std::thread g_DumpThread;
static std::atomic<bool> g_DumpRunning{ false };
static std::atomic<bool> g_DumpAccepting{ true };

static constexpr size_t kMaxRetainedJobs = 50;

static int64_t NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string MakeJobId()
{
	int id = ++g_JobCounter;
	return "job-" + std::to_string(id);
}

static json JobToJson(const DumpJob& job)
{
	json j;
	j["id"] = job.Id;
	j["format"] = job.Format;
	j["status"] = job.Status == JobStatus::Running ? "running"
		: job.Status == JobStatus::Completed ? "completed" : "failed";
	j["output_path"] = job.OutputPath;
	j["start_time"] = job.StartTime;
	j["end_time"] = job.EndTime;
	j["duration_ms"] = job.EndTime > 0 ? (job.EndTime - job.StartTime) : (NowMs() - job.StartTime);
	if (!job.Error.empty()) j["error"] = job.Error;
	return j;
}

static void PruneOldJobs()
{
	if (g_Jobs.size() <= kMaxRetainedJobs)
		return;

	std::string oldestId;
	int64_t oldestTime = INT64_MAX;
	for (const auto& [id, job] : g_Jobs)
	{
		if (job.Status == JobStatus::Running)
			continue;
		if (job.EndTime < oldestTime)
		{
			oldestTime = job.EndTime;
			oldestId = id;
		}
	}
	if (!oldestId.empty())
		g_Jobs.erase(oldestId);
}

struct DumpLaunchResult
{
	bool Started = false;
	std::string JobId;
	std::string Error;
};

class DumpRunningGuard
{
public:
	~DumpRunningGuard()
	{
		g_DumpRunning.store(false, std::memory_order_release);
		g_DumpStoppedCV.notify_all();
	}
};

static void MarkJobFailedNoThrow(const std::string& jobId, const std::string& error) noexcept
{
	try
	{
		std::lock_guard<std::mutex> lock(g_JobsMutex);
		auto it = g_Jobs.find(jobId);
		if (it != g_Jobs.end())
		{
			it->second.Status = JobStatus::Failed;
			it->second.EndTime = NowMs();
			it->second.Error = error;
		}
	}
	catch (...) {}
}

template<typename GeneratorType>
static DumpLaunchResult LaunchGeneratorJob(const std::string& format)
{
	std::unique_lock<std::mutex> threadLock(g_ThreadsMutex);
	if (!g_DumpAccepting.load(std::memory_order_acquire))
		return { false, {}, "DUMP_EXECUTOR_STOPPING" };
	if (g_DumpRunning.load(std::memory_order_acquire))
		return { false, {}, "DUMP_EXECUTOR_BUSY" };
	if (g_DumpThread.joinable())
		g_DumpThread.join();

	std::string jobId = MakeJobId();

	DumpJob job;
	job.Id = jobId;
	job.Format = format;
	job.Status = JobStatus::Running;
	job.StartTime = NowMs();

	{
		std::lock_guard<std::mutex> lock(g_JobsMutex);
		PruneOldJobs();
		g_Jobs[jobId] = job;
	}

	g_DumpRunning.store(true, std::memory_order_release);
	try
	{
		g_DumpThread = std::thread([jobId]() {
		DumpRunningGuard runningGuard;
		try {
			Generator::Generate<GeneratorType>();

			std::lock_guard<std::mutex> lock(g_JobsMutex);
			auto it = g_Jobs.find(jobId);
			if (it == g_Jobs.end())
				return;
			auto& j = it->second;
			j.Status = JobStatus::Completed;
			j.EndTime = NowMs();
			j.OutputPath = Generator::GetDumperFolder().string()
				+ "/" + GeneratorType::MainFolderName;
		}
		catch (const std::exception& e) {
			MarkJobFailedNoThrow(jobId, e.what());
		}
		catch (...) {
			MarkJobFailedNoThrow(jobId, "Unknown error");
		}
		});
	}
	catch (const std::exception& error)
	{
		g_DumpRunning.store(false, std::memory_order_release);
		MarkJobFailedNoThrow(jobId, error.what());
		return { false, jobId, "DUMP_THREAD_CREATE_FAILED" };
	}

	return { true, jobId, {} };
}

bool ShutdownDumpJobs(int timeoutMs)
{
	g_DumpAccepting.store(false, std::memory_order_release);
	std::unique_lock<std::mutex> lock(g_ThreadsMutex);
	const bool stopped = g_DumpStoppedCV.wait_for(
		lock,
		std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 0),
		[] { return !g_DumpRunning.load(std::memory_order_acquire); });
	if (!stopped)
		return false;
	if (g_DumpThread.joinable())
		g_DumpThread.join();
	return true;
}

static bool ValidateNoDumpOptions(const HttpRequest& req, std::string& outError)
{
	try
	{
		const json body = req.Body.empty() ? json::object() : json::parse(req.Body);
		if (!body.is_object())
		{
			outError = "DUMP_OPTIONS_MUST_BE_OBJECT";
			return false;
		}
		if (!body.empty())
		{
			outError = "DUMP_OPTIONS_UNAVAILABLE";
			return false;
		}
		return true;
	}
	catch (const json::exception& error)
	{
		outError = std::string("INVALID_DUMP_OPTIONS_JSON: ") + error.what();
		return false;
	}
}

template<typename GeneratorType>
static HttpResponse StartGenerator(const HttpRequest& req, const std::string& format, const std::string& message)
{
	std::string validationError;
	if (!ValidateNoDumpOptions(req, validationError))
		return { 400, "application/json", MakeError(validationError) };

	const DumpLaunchResult launch = LaunchGeneratorJob<GeneratorType>(format);
	if (!launch.Started)
		return { 409, "application/json", MakeError(launch.Error) };

	json data;
	data["job_id"] = launch.JobId;
	data["message"] = message;
	return { 200, "application/json", MakeResponse(data) };
}

void RegisterDumpRoutes(HttpServer& server)
{
	server.Post("/api/v1/dump/sdk", [](const HttpRequest& req) -> HttpResponse {
		return StartGenerator<CppGenerator>(req, "sdk", "C++ SDK generation started");
	});

	server.Post("/api/v1/dump/usmap", [](const HttpRequest& req) -> HttpResponse {
		return StartGenerator<MappingGenerator>(req, "usmap", "USMAP generation started");
	});

	server.Post("/api/v1/dump/dumpspace", [](const HttpRequest& req) -> HttpResponse {
		return StartGenerator<DumpspaceGenerator>(req, "dumpspace", "Dumpspace generation started");
	});

	server.Post("/api/v1/dump/ida-script", [](const HttpRequest& req) -> HttpResponse {
		return StartGenerator<IDAMappingGenerator>(req, "ida-script", "IDA mapping generation started");
	});

	server.Get("/api/v1/dump/jobs", [](const HttpRequest& req) -> HttpResponse {
		std::lock_guard<std::mutex> lock(g_JobsMutex);
		json items = json::array();
		for (const auto& [id, job] : g_Jobs)
			items.push_back(JobToJson(job));
		return { 200, "application/json", MakeResponse(items) };
	});

	server.Get("/api/v1/dump/jobs/:id", [](const HttpRequest& req) -> HttpResponse {
		std::string id = GetPathSegment(req.Path, 4);
		if (id.empty())
			return { 400, "application/json", MakeError("Missing job ID") };

		std::lock_guard<std::mutex> lock(g_JobsMutex);
		auto it = g_Jobs.find(id);
		if (it == g_Jobs.end())
			return { 404, "application/json", MakeError("Job not found: " + id) };

		return { 200, "application/json", MakeResponse(JobToJson(it->second)) };
	});
}

} // namespace UExplorer::API
