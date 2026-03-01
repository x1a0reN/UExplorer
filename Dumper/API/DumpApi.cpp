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
#include <chrono>

namespace UExplorer::API
{

// ---- Job tracking ----

enum class JobStatus { Running, Completed, Failed };

struct DumpJob
{
	std::string Id;
	std::string Format;     // "sdk", "usmap", "dumpspace", "ida-script"
	JobStatus Status = JobStatus::Running;
	std::string Error;
	std::string OutputPath;
	int64_t StartTime = 0;
	int64_t EndTime = 0;
};

static std::mutex g_JobsMutex;
static std::unordered_map<std::string, DumpJob> g_Jobs;
static std::atomic<int> g_JobCounter{ 0 };

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

// Run a generator in a background thread, tracking via job ID
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
		g_Jobs[jobId] = job;
	}

	std::thread([jobId]() {
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
	}).detach();

	return jobId;
}

void RegisterDumpRoutes(HttpServer& server)
{
	// POST /api/v1/dump/sdk — generate C++ SDK
	server.Post("/api/v1/dump/sdk", [](const HttpRequest& req) -> HttpResponse {
		std::string jobId = LaunchGeneratorJob<CppGenerator>("sdk");
		json data;
		data["job_id"] = jobId;
		data["message"] = "C++ SDK generation started";
		return { 200, "application/json", MakeResponse(data) };
	});

	// POST /api/v1/dump/usmap — generate USMAP mappings
	server.Post("/api/v1/dump/usmap", [](const HttpRequest& req) -> HttpResponse {
		std::string jobId = LaunchGeneratorJob<MappingGenerator>("usmap");
		json data;
		data["job_id"] = jobId;
		data["message"] = "USMAP generation started";
		return { 200, "application/json", MakeResponse(data) };
	});

	// POST /api/v1/dump/dumpspace — generate Dumpspace JSON
	server.Post("/api/v1/dump/dumpspace", [](const HttpRequest& req) -> HttpResponse {
		std::string jobId = LaunchGeneratorJob<DumpspaceGenerator>("dumpspace");
		json data;
		data["job_id"] = jobId;
		data["message"] = "Dumpspace generation started";
		return { 200, "application/json", MakeResponse(data) };
	});

	// POST /api/v1/dump/ida-script — generate IDA mapping
	server.Post("/api/v1/dump/ida-script", [](const HttpRequest& req) -> HttpResponse {
		std::string jobId = LaunchGeneratorJob<IDAMappingGenerator>("ida-script");
		json data;
		data["job_id"] = jobId;
		data["message"] = "IDA mapping generation started";
		return { 200, "application/json", MakeResponse(data) };
	});

	// GET /api/v1/dump/jobs — list all jobs
	server.Get("/api/v1/dump/jobs", [](const HttpRequest& req) -> HttpResponse {
		std::lock_guard<std::mutex> lock(g_JobsMutex);
		json items = json::array();
		for (const auto& [id, job] : g_Jobs)
			items.push_back(JobToJson(job));
		return { 200, "application/json", MakeResponse(items) };
	});

	// GET /api/v1/dump/jobs/:id — get job status
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