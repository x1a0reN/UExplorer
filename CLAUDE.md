
# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

UExplorer is a desktop tool for Unreal Engine SDK dumping and live game exploration. It consists of:
- **Core DLL** (C++): Runs inside the target game process, provides memory access and UE object introspection
- **HTTP Server**: Built into the DLL, serves REST API for frontend communication
- **Frontend** (Tauri + React + TypeScript): Desktop GUI for exploring game data (not yet implemented)

## Rules

1. **NEVER execute any delete commands. Deleting any files is strictly prohibited!**
2. **When testing APIs, strictly use curl by default, unless the user explicitly requests the use of chrome-mcp.**
3. **After every code modification, you must clearly inform the user: exactly what was modified, the motive behind the change, the specific problem it solved, and the root cause that necessitated the modification.**
4. **After modifying the code, you must push the changes to GitHub and then compile using the following command: powershell -Command "& 'D:\Program Files\Visual Studio 2026\MSBuild\Current\Bin\MSBuild.exe' 'D:\Projects\UExplorer\Dumper\UExplorerCore.vcxproj' /p:Configuration=Release /p:Platform=x64 /m:1 /v:minimal 2>&1"**
5. **After every code modification, you must review DESIGN.md and update the current project progress within it.**


## Build Commands

```bash
# Build the DLL
cd Dumper
"D:\Program Files\Visual Studio 2026\MSBuild\Current\Bin\MSBuild.exe" UExplorerCore.vcxproj -p:Configuration=Release -p:Platform=x64 -t:Build

# Output: Dumper\x64\Release\UExplorerCore.dll
```

## Architecture

### Core Components

1. **Main.cpp** - DLL entry point
   - Initializes UE engine core (GObjects, GNames, offsets)
   - Starts HTTP server on configurable port (default 27015)
   - Handles F6 key to gracefully unload

2. **Server/** - HTTP server implementation
   - `HttpServer.h/cpp`: WinSock2-based HTTP server with route matching
   - Supports REST API routes and SSE (Server-Sent Events) for real-time updates
   - Token-based authentication via `X-UExplorer-Token` header

3. **API/** - REST API endpoints (13 modules)
   - `StatusApi.cpp`: Connection status, health check
   - `ObjectsApi.cpp`: Object enumeration, search, properties
   - `ClassesApi.cpp`: Class/struct hierarchy, fields, functions, instances, CDO
   - `EnumsApi.cpp`: Enum values
   - `DumpApi.cpp`: SDK generation (C++, USMAP, Dumpspace, IDA)
   - `MemoryApi.cpp`: Raw memory R/W, typed R/W, pointer chains
   - `WorldApi.cpp`: UWorld, actors, shortcuts
   - `CallApi.cpp`: Function invocation via ProcessEvent
   - `BlueprintApi.cpp`: Bytecode decompilation
   - `WatchApi.cpp`: Property watch with change detection
   - `HookApi.cpp`: UFunction hooking via PostRender VTable
   - `EventsApi.cpp`: SSE event broadcasting

4. **Engine/** - Unreal Engine introspection (from Dumper-7)
   - ObjectArray: GObjects enumeration
   - NameArray: FName handling
   - UnrealObjects: UE object wrappers
   - UnrealTypes: FString, FName, TArray, etc.
   - OffsetFinder: Auto-discovery of engine offsets

5. **Generator/** - SDK generation (from Dumper-7)
   - CppGenerator: C++ SDK headers
   - MappingGenerator: USMAP format
   - DumpspaceGenerator: JSON dumps
   - IDAMappingGenerator: IDA Pro scripts

### Key Patterns

- **ProcessEvent Calling**: Both static and non-static functions use VTable-based ProcessEvent calls
  - Static functions: Call via Class Default Object (CDO)
  - Non-static functions: Call via object instance VTable
- **Game Thread Dispatch**: Use `GameThreadQueue.h` to dispatch calls to game thread (PostRender hook)
- **VTable Hooking**: Hook `GameViewportClient::PostRender` (index 98) instead of inline ProcessEvent to avoid detection

### API Authentication

All endpoints (except `/api/v1/status/health`) require header:
```
X-UExplorer-Token: uexplorer-dev
```

### Token

Development token: `uexplorer-dev` (hardcoded in Main.cpp)

## Important Implementation Details

- The DLL runs inside the target game process
- HTTP server listens on 127.0.0.1 (localhost only)
- SSE endpoints for real-time: `/api/v1/events/stream`, `/api/v1/events/watches`, `/api/v1/events/hooks`
- F6 key triggers graceful shutdown (unhooks, stops server, unloads DLL)
- Offset discovery happens at startup via pattern scanning

## Known Issues & Workarounds

### VS2026 Compilation Issues

1. **VirtualQuery not found** (error C3861)
   - **Problem**: `VirtualQuery` Windows API cannot be found even with `<windows.h>` included
   - **Root Cause**: VS2026 preview compiler has issues with some Windows API calls in this context
   - **Workaround**: Replace `VirtualQuery` with try-catch memory access approach in `PlatformWindows.cpp::IsBadReadPtr`

2. **zstd.c STL compilation error** (error STL1003)
   - **Problem**: zstd.c is incorrectly compiled as C++ instead of C, causing STL header conflicts
   - **Root Cause**: VS2026 preview has issues with C/C++ mixed compilation
   - **Workaround**:
     - Exclude `zstd.c` from project file (UExplorerCore.vcxproj)
     - Disable ZStandard compression in `MappingGenerator.cpp` (ZSTD functions commented out)
   - **Note**: USMAP files will be generated without compression

### Build Commands

```powershell
# Build with VS2026
"D:\Program Files\Visual Studio 2026\MSBuild\Current\Bin\MSBuild.exe" UExplorerCore.vcxproj -p:Configuration=Release -p:Platform=x64
```
