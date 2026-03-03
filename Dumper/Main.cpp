#include <Windows.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstring>

#include "Generators/Generator.h"
#include "Server/HttpServer.h"
#include "API/Router.h"
#include "API/HookApi.h"
#include "Settings.h"
#include "OffsetFinder/Offsets.h"

static std::atomic<bool> g_Running{ true };
static std::unique_ptr<UExplorer::HttpServer> g_Server;
static HMODULE g_Module = nullptr;

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

	// Start HTTP server
	const uint16_t port = 27015; // TODO: read from config
	const std::string token = "uexplorer-dev"; // TODO: generate random token

	try
	{
		g_Server = std::make_unique<UExplorer::HttpServer>(port, token);
		UExplorer::API::RegisterAllRoutes(*g_Server);
		if (!g_Server->Start())
		{
			std::cerr << "[UExplorer] Failed to start HTTP server (all ports failed)\n";
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
