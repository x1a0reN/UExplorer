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
#include <unordered_map>
#include <vector>
#include <chrono>
#include <algorithm>

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
static std::vector<std::thread> g_DumpThreads;

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

template<typename GeneratorType>
static std::string LaunchGeneratorJob(const std::string& format)
{
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

	std::thread t([jobId]() {
		try {
			Generator::Generate<GeneratorType>();

			std::lock_guard<std::mutex> lock(g_JobsMutex);
			auto& j = g_Jobs[jobId];
			j.Status = JobStatus::Completed;
			j.EndTime = NowMs();
			j.OutputPath = Generator::GetDumperFolder().string()
				+ "/" + GeneratorType::MainFolderName;
		}
		catch (const std::exception& e) {
			std::lock_guard<std::mutex> lock(g_JobsMutex);
			auto& j = g_Jobs[jobId];
			j.Status = JobStatus::Failed;
			j.EndTime = NowMs();
			j.Error = e.what();
		}
		catch (...) {
			std::lock_guard<std::mutex> lock(g_JobsMutex);
			auto& j = g_Jobs[jobId];
			j.Status = JobStatus::Failed;
			j.EndTime = NowMs();
			j.Error = "Unknown error";
		}
	});

	{
		std::lock_guard<std::mutex> lk(g_ThreadsMutex);
		// Clean up finished threads
		g_DumpThreads.erase(
			std::remove_if(g_DumpThreads.begin(), g_DumpThreads.end(),
				[](std::thread& th) {
					if (th.joinable()) {
						th.join();
						return true;
					}
					return true;
				}),
			g_DumpThreads.end());
		g_DumpThreads.push_back(std::move(t));
	}

	return jobId;
}

void ShutdownDumpJobs()
{
	std::lock_guard<std::mutex> lk(g_ThreadsMutex);
	for (auto& th : g_DumpThreads)
	{
		if (th.joinable())
			th.join();
	}
	g_DumpThreads.clear();
}

void RegisterDumpRoutes(HttpServer& server)
{
	server.Post("/api/v1/dump/sdk", [](const HttpRequest& req) -> HttpResponse {
		std::string jobId = LaunchGeneratorJob<CppGenerator>("sdk");
		json data;
		data["job_id"] = jobId;
		data["message"] = "C++ SDK generation started";
		return { 200, "application/json", MakeResponse(data) };
	});

	server.Post("/api/v1/dump/usmap", [](const HttpRequest& req) -> HttpResponse {
		std::string jobId = LaunchGeneratorJob<MappingGenerator>("usmap");
		json data;
		data["job_id"] = jobId;
		data["message"] = "USMAP generation started";
		return { 200, "application/json", MakeResponse(data) };
	});

	server.Post("/api/v1/dump/dumpspace", [](const HttpRequest& req) -> HttpResponse {
		std::string jobId = LaunchGeneratorJob<DumpspaceGenerator>("dumpspace");
		json data;
		data["job_id"] = jobId;
		data["message"] = "Dumpspace generation started";
		return { 200, "application/json", MakeResponse(data) };
	});

	server.Post("/api/v1/dump/ida-script", [](const HttpRequest& req) -> HttpResponse {
		std::string jobId = LaunchGeneratorJob<IDAMappingGenerator>("ida-script");
		json data;
		data["job_id"] = jobId;
		data["message"] = "IDA mapping generation started";
		return { 200, "application/json", MakeResponse(data) };
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
