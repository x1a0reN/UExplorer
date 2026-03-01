#pragma once

// Forward declarations for Windows memory APIs
// Avoids WIN32_LEAN_AND_MEAN conflicts with <filesystem>

#ifndef _WINDEF_
typedef unsigned long DWORD;
typedef int BOOL;
typedef void* LPVOID;
typedef const void* LPCVOID;
typedef DWORD* PDWORD;
typedef size_t SIZE_T;
#endif

#ifndef PAGE_EXECUTE_READWRITE
#define PAGE_EXECUTE_READWRITE 0x40
#endif
#ifndef MEM_COMMIT
#define MEM_COMMIT 0x00001000
#endif
#ifndef MEM_RESERVE
#define MEM_RESERVE 0x00002000
#endif
#ifndef MEM_RELEASE
#define MEM_RELEASE 0x00008000
#endif

extern "C" {
__declspec(dllimport) BOOL __stdcall VirtualProtect(LPVOID lpAddress, SIZE_T dwSize, DWORD flNewProtect, PDWORD lpflOldProtect);
__declspec(dllimport) LPVOID __stdcall VirtualAlloc(LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect);
__declspec(dllimport) BOOL __stdcall VirtualFree(LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType);
}
