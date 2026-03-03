#include <Windows.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <cctype>

#include "Generators/Generator.h"
#include "Server/HttpServer.h"
#include "API/Router.h"
#include "API/HookApi.h"
#include "Settings.h"
#include "OffsetFinder/Offsets.h"

static std::atomic<bool> g_Running{ true };
static std::unique_ptr<UExplorer::HttpServer> g_Server;
static HMODULE g_Module = nullptr;

namespace
{
	constexpr uint16_t kDefaultApiPort = 27015;
	constexpr const char* kDefaultApiToken = "uexplorer-dev";

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

	void EnsureDefaultConnectionConfig(const std::string& path)
	{
		if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES)
			return;

		WritePrivateProfileStringA("Connection", "PreferredPort", std::to_string(kDefaultApiPort).c_str(), path.c_str());
		WritePrivateProfileStringA("Connection", "Token", kDefaultApiToken, path.c_str());
		WritePrivateProfileStringA("Connection", "PortMode", "fixed", path.c_str());
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

DWORD MainThread(HMODULE Module)
{
	AllocConsole();
	FILE* Dummy;
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

	// Initialize Unreal Engine core (GObjects, GNames, offsets)
	Generator::InitEngineCore();
	Generator::InitInternal();

	std::cerr << "[UExplorer] Engine core initialized.\n";

	// Retrieve game info via ProcessEvent
	if (Settings::Generator::GameName.empty() && Settings::Generator::GameVersion.empty())
	{
		FString Name;
		FString Version;
		UEClass Kismet = ObjectArray::FindClassFast("KismetSystemLibrary");
		UEFunction GetGameName = Kismet.GetFunction("KismetSystemLibrary", "GetGameName");
		UEFunction GetEngineVersion = Kismet.GetFunction("KismetSystemLibrary", "GetEngineVersion");

		Kismet.ProcessEvent(GetGameName, &Name);
		Kismet.ProcessEvent(GetEngineVersion, &Version);

		Settings::Generator::GameName = Name.ToString();
		Settings::Generator::GameVersion = Version.ToString();
	}

	std::cerr << "[UExplorer] Game: " << Settings::Generator::GameName << "\n";
	std::cerr << "[UExplorer] Version: " << Settings::Generator::GameVersion << "\n";

	uint16_t configuredPort = kDefaultApiPort;
	std::string configuredToken = kDefaultApiToken;
	LoadConnectionConfig(configuredPort, configuredToken);
	std::cerr << "[UExplorer] Connection config: preferred_port="
		<< configuredPort << " token_len=" << configuredToken.size() << "\n";

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

	// Cleanup
	std::cerr << "[UExplorer] Shutting down...\n";

	// Shutdown hooks first (before server stops, while game thread is still running)
	UExplorer::API::ShutdownHooks();

	if (g_Server) g_Server->Stop();
	WriteRuntimeState(0, token, false);

	fclose(stderr);
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
		CreateThread(0, 0, (LPTHREAD_START_ROUTINE)MainThread, hModule, 0, 0);
		break;
	}
	return TRUE;
}
