#include <Windows.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <random>

#include "Generators/Generator.h"
#include "Server/HttpServer.h"
#include "API/Router.h"
#include "API/HookApi.h"
#include "API/EventsApi.h"
#include "API/DumpApi.h"
#include "Settings.h"
#include "OffsetFinder/Offsets.h"

static std::atomic<bool> g_Running{ true };
static std::unique_ptr<UExplorer::HttpServer> g_Server;
static HMODULE g_Module = nullptr;

namespace
{
	constexpr uint16_t kDefaultApiPort = 27015;
	constexpr const char* kDefaultApiToken = "uexplorer-dev";
	constexpr const char* kAuthMode = "token_header";

	std::string Trim(std::string value)
	{
		auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
		value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
		value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
		return value;
	}

	std::string GetLocalAppDataPath()
	{
		char buffer[MAX_PATH] = {};
		const DWORD len = GetEnvironmentVariableA("LOCALAPPDATA", buffer, MAX_PATH);
		if (len > 0 && len < MAX_PATH)
			return std::string(buffer, len);
		return {};
	}

	std::string GetUExplorerConfigDir()
	{
		namespace fs = std::filesystem;
		std::string base = GetLocalAppDataPath();
		if (base.empty())
		{
			char tempBuf[MAX_PATH] = {};
			const DWORD len = GetTempPathA(MAX_PATH, tempBuf);
			base.assign(tempBuf, len > 0 ? len : 0);
		}

		if (base.empty())
			base = ".";

		fs::path dir = fs::path(base) / "UExplorer";
		std::error_code ec;
		fs::create_directories(dir, ec);
		return dir.string();
	}

	std::string GetConnectionConfigPath()
	{
		return (std::filesystem::path(GetUExplorerConfigDir()) / "connection.ini").string();
	}

	std::string GetRuntimeStatePath()
	{
		return (std::filesystem::path(GetUExplorerConfigDir()) / "runtime.ini").string();
	}

	std::string GenerateRandomToken(size_t length = 40)
	{
		static constexpr char kAlphabet[] =
			"0123456789"
			"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
			"abcdefghijklmnopqrstuvwxyz";
		static constexpr size_t kAlphabetLen = sizeof(kAlphabet) - 1;

		std::random_device rd;
		std::mt19937_64 gen(rd());
		std::uniform_int_distribution<size_t> dist(0, kAlphabetLen - 1);

		std::string token;
		token.reserve(length);
		for (size_t i = 0; i < length; i++)
		{
			token.push_back(kAlphabet[dist(gen)]);
		}
		return token;
	}

	void EnsureDefaultConnectionConfig(const std::string& path)
	{
		const bool exists = (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES);
		if (!exists)
		{
			WritePrivateProfileStringA("Connection", "PreferredPort", std::to_string(kDefaultApiPort).c_str(), path.c_str());
			WritePrivateProfileStringA("Connection", "PortMode", "fixed", path.c_str());
		}

		char tokenBuf[256] = {};
		GetPrivateProfileStringA("Connection", "Token", "", tokenBuf, static_cast<DWORD>(sizeof(tokenBuf)), path.c_str());
		std::string token = Trim(tokenBuf);
		if (token.empty() || token == kDefaultApiToken)
		{
			token = GenerateRandomToken();
			WritePrivateProfileStringA("Connection", "Token", token.c_str(), path.c_str());
			std::cerr << "[UExplorer] Connection token generated and persisted (len=" << token.size() << ")\n";
		}
	}

	void LoadConnectionConfig(uint16_t& outPort, std::string& outToken)
	{
		const std::string cfgPath = GetConnectionConfigPath();
		EnsureDefaultConnectionConfig(cfgPath);

		int32_t preferred = GetPrivateProfileIntA("Connection", "PreferredPort", kDefaultApiPort, cfgPath.c_str());
		if (preferred < 0 || preferred > 65535)
			preferred = kDefaultApiPort;

		char modeBuf[32] = {};
		GetPrivateProfileStringA("Connection", "PortMode", "fixed", modeBuf, static_cast<DWORD>(sizeof(modeBuf)), cfgPath.c_str());
		std::string mode = Trim(modeBuf);
		std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

		char tokenBuf[256] = {};
		GetPrivateProfileStringA("Connection", "Token", kDefaultApiToken, tokenBuf, static_cast<DWORD>(sizeof(tokenBuf)), cfgPath.c_str());
		std::string token = Trim(tokenBuf);
		if (token.empty())
			token = kDefaultApiToken;

		outPort = (mode == "auto") ? 0 : static_cast<uint16_t>(preferred);
		outToken = token;
	}

	void WriteRuntimeState(uint16_t port, const std::string& token, bool running)
	{
		const std::string runtimePath = GetRuntimeStatePath();
		WritePrivateProfileStringA("Runtime", "Port", std::to_string(port).c_str(), runtimePath.c_str());
		WritePrivateProfileStringA("Runtime", "Token", token.c_str(), runtimePath.c_str());
		WritePrivateProfileStringA("Runtime", "AuthMode", kAuthMode, runtimePath.c_str());
		WritePrivateProfileStringA("Runtime", "Pid", std::to_string(GetCurrentProcessId()).c_str(), runtimePath.c_str());
		WritePrivateProfileStringA("Runtime", "Running", running ? "1" : "0", runtimePath.c_str());
	}
}

static std::string TryProbeEngineVersionFromImage()
{
	HMODULE exeModule = GetModuleHandleW(nullptr);
	if (!exeModule)
		return "";

	const uint8* base = reinterpret_cast<const uint8*>(exeModule);
	const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
	if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE)
		return "";

	if (dos->e_lfanew <= 0 || dos->e_lfanew > 0x10000)
		return "";

	const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
	if (!nt || nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.SizeOfImage == 0)
		return "";

	const char* begin = reinterpret_cast<const char*>(base);
	const char* end = begin + nt->OptionalHeader.SizeOfImage;

	auto ScanWithTag = [begin, end](const char* tag) -> std::string
	{
		const size_t tagLen = std::strlen(tag);
		if (tagLen == 0 || static_cast<size_t>(end - begin) <= tagLen)
			return "";

		for (const char* p = begin; p + static_cast<ptrdiff_t>(tagLen + 4) < end; ++p)
		{
			if (std::memcmp(p, tag, tagLen) != 0)
				continue;

			const char* v = p + tagLen;
			std::string parsed;
			while (v < end)
			{
				const char ch = *v;
				if ((ch >= '0' && ch <= '9') || ch == '.')
				{
					parsed.push_back(ch);
					if (parsed.size() >= 16)
						break;
					++v;
					continue;
				}
				break;
			}

			if (parsed.find('.') != std::string::npos)
				return parsed;
		}

		return "";
	};

	std::string version = ScanWithTag("++UE4+Release-");
	if (version.empty())
		version = ScanWithTag("++UE5+Release-");
	if (version.empty())
		version = ScanWithTag("UE4+Release-");
	if (version.empty())
		version = ScanWithTag("UE5+Release-");

	return version;
}

static void PrimeGameVersionBeforeOffsetInit()
{
	if (!Settings::Generator::GameVersion.empty())
		return;

	const std::string probed = TryProbeEngineVersionFromImage();
	if (!probed.empty())
	{
		Settings::Generator::GameVersion = probed;
		std::cerr << "[UExplorer] Pre-init engine version probe: " << Settings::Generator::GameVersion << "\n";
	}
	else
	{
		std::cerr << "[UExplorer] Pre-init engine version probe: not found\n";
	}
}

static DWORD WINAPI MainThread(LPVOID lpParam)
{
	HMODULE Module = reinterpret_cast<HMODULE>(lpParam);

	AllocConsole();
	FILE* Dummy = nullptr;
	freopen_s(&Dummy, "CONOUT$", "w", stderr);
	freopen_s(&Dummy, "CONIN$", "r", stdin);

	std::cerr << "[UExplorer] Initializing...\n";

	Settings::Config::Load(Module);

	if (Settings::Config::SleepTimeout > 0)
	{
		std::cerr << "[UExplorer] Sleeping for " << Settings::Config::SleepTimeout << "ms...\n";
		Sleep(Settings::Config::SleepTimeout);
	}

	PrimeGameVersionBeforeOffsetInit();

	try
	{
		Generator::InitEngineCore();
		Generator::InitInternal();
	}
	catch (const std::exception& e)
	{
		std::cerr << "[UExplorer] FATAL: Engine init failed: " << e.what() << "\n";
		std::cerr << "[UExplorer] DLL will remain loaded but non-functional.\n";
		if (Dummy) fclose(Dummy);
		FreeConsole();
		FreeLibraryAndExitThread(Module, 1);
		return 1;
	}
	catch (...)
	{
		std::cerr << "[UExplorer] FATAL: Engine init crashed with unknown exception.\n";
		if (Dummy) fclose(Dummy);
		FreeConsole();
		FreeLibraryAndExitThread(Module, 1);
		return 1;
	}

	std::cerr << "[UExplorer] Engine core initialized.\n";

	if (Settings::Generator::GameName.empty() && Settings::Generator::GameVersion.empty())
	{
		try
		{
			FString Name;
			FString Version;
			UEClass Kismet = ObjectArray::FindClassFast("KismetSystemLibrary");
			if (Kismet)
			{
				UEFunction GetGameName = Kismet.GetFunction("KismetSystemLibrary", "GetGameName");
				UEFunction GetEngineVersion = Kismet.GetFunction("KismetSystemLibrary", "GetEngineVersion");

				if (GetGameName) Kismet.ProcessEvent(GetGameName, &Name);
				if (GetEngineVersion) Kismet.ProcessEvent(GetEngineVersion, &Version);

				Settings::Generator::GameName = Name.ToString();
				Settings::Generator::GameVersion = Version.ToString();
			}
			else
			{
				std::cerr << "[UExplorer] Warning: KismetSystemLibrary not found, game info unavailable.\n";
			}
		}
		catch (...)
		{
			std::cerr << "[UExplorer] Warning: Failed to query game info via ProcessEvent.\n";
		}
	}

	std::cerr << "[UExplorer] Game: " << Settings::Generator::GameName << "\n";
	std::cerr << "[UExplorer] Version: " << Settings::Generator::GameVersion << "\n";

	uint16_t configuredPort = kDefaultApiPort;
	std::string configuredToken = kDefaultApiToken;
	LoadConnectionConfig(configuredPort, configuredToken);
	std::cerr << "[UExplorer] Connection config: preferred_port="
		<< configuredPort << " token_len=" << configuredToken.size()
		<< " auth_mode=" << kAuthMode << "\n";

	// Start HTTP server
	const uint16_t port = configuredPort;
	const std::string token = configuredToken;

	try
	{
		g_Server = std::make_unique<UExplorer::HttpServer>(port, token);
		UExplorer::API::RegisterAllRoutes(*g_Server);
		if (!g_Server->Start())
		{
			std::cerr << "[UExplorer] Failed to start HTTP server (all ports failed)\n";
		}
		else
		{
			const uint16_t actualPort = g_Server->GetPort();
			if (configuredPort != 0 && actualPort != configuredPort)
			{
				std::cerr << "[UExplorer] Preferred port " << configuredPort
					<< " unavailable, switched to " << actualPort << "\n";
			}
			WriteRuntimeState(actualPort, token, true);
		}
	}
	catch (const std::exception& e)
	{
		std::cerr << "[UExplorer] Failed to start HTTP server: " << e.what() << "\n";
	}

	// Keep alive, F6 to unload
	while (g_Running.load())
	{
		if (GetAsyncKeyState(VK_F6) & 1)
		{
			g_Running.store(false);
			break;
		}
		Sleep(100);
	}

	std::cerr << "[UExplorer] Shutting down...\n";

	UExplorer::API::ShutdownHooks();
	UExplorer::API::ShutdownDumpJobs();
	UExplorer::API::SetServer(nullptr);

	if (g_Server) g_Server->Stop();
	g_Server.reset();
	WriteRuntimeState(0, token, false);

	if (Dummy) fclose(Dummy);
	FreeConsole();
	FreeLibraryAndExitThread(Module, 0);

	return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		g_Module = hModule;
		CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr);
		break;
	}
	return TRUE;
}
