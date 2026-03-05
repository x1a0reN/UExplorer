---
trigger: always_on
glob:
description: Project-specific rules for UExplorer
---

## Rules

1. **NEVER execute any delete commands. Deleting any files is strictly prohibited!**
2. **When testing APIs, strictly use curl by default, unless the user explicitly requests the use of chrome-mcp.**
3. **After every code modification, you must clearly inform the user: exactly what was modified, the motive behind the change, the specific problem it solved, and the root cause that necessitated the modification.**
4. **After modifying DLL code, you must push the changes to GitHub and then compile using the following command: `powershell -Command "& 'D:\Program Files\Visual Studio 2026\MSBuild\Current\Bin\MSBuild.exe' 'D:\Projects\UExplorer\Dumper\UExplorerCore.vcxproj' /p:Configuration=Release /p:Platform=x64 /m:1 /v:minimal 2>&1"`**
5. **After every code modification, you must review DESIGN.md and update the current project progress within it.**
6. **No fallback strategies. If something works, it works. If it doesn't, it doesn't. Never offer a lesser alternative as a fallback.**
7. **After modifying frontend (client-side) code, you must build the Tauri client WITHOUT the MSI installer dependency, using: `cd frontend && npx tauri build --no-bundle`**
8. **After every code modification, commit and push changes to GitHub in Chinese. Do not add any author signature to the commit message.**
