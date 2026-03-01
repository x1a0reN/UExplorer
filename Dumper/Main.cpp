#include <Windows.h>
#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>

#include "Generators/Generator.h"
#include "Server/HttpServer.h"
#include "API/Router.h"
#include "Settings.h"

static std::atomic<bool> g_Running{ true };
static std::unique_ptr<UExplorer::HttpServer> g_Server;
static HMODULE g_Module = nullptr;

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
