# UExplorer

UExplorer is a Windows desktop tool for Unreal Engine SDK generation and live runtime
inspection. It combines an injected C++ Core with a Tauri/React desktop application.

The project is undergoing a safety and architecture refactor. The desktop transport
cutover is complete: React reaches the injected Core only through the Tauri/Rust Host
and a PID-scoped Windows Named Pipe. The old in-DLL HTTP/SSE/WebSocket sources are
retained as historical evidence, but they are excluded from the release Core project
and have no runtime entry point.

## Target architecture

```text
React UI
  -> Tauri commands and events
Rust Host
  -> versioned Windows Named Pipe RPC
Core DLL inside the selected game process
  -> Unreal Engine reflection and game-thread capabilities
```

The Core DLL does not expose HTTP, SSE, or WebSocket. No external gateway is currently
shipped. If one is added later, it belongs to the Rust Host and must use the same
`DomainService` as the desktop UI. Protocol or capability failures are explicit; there
is no silent transport, offset, thread, or compression fallback.

## Current status

- Refactor plan: `REFACTOR_PLAN.md`
- Current design truth and progress: `DESIGN.md` section 0
- Supported runtime evidence: `docs/SUPPORT_MATRIX.md`
- IPC boundary decision: `docs/adr/0001-process-boundary-and-ipc.md`
- Issue workflow state: `docs/issue-status.json`
- Legacy API snapshot: `tests/contracts/api-v1-routes.tsv`

The C++ Core owns a real PID-scoped Windows Named Pipe server with current-user
ACL/PID verification, bounded RPC workers, cancellation, and joinable shutdown. The
Rust Host has a real overlapped `CoreRpcClient` with pre-Hello server-PID checks,
bounded request/event queues, deadlines, cancellation, disconnect completion,
explicit reconnect, and joinable shutdown. EventHub, multi-PID SessionManager, strict
injection-to-Core-Ready gating, cross-language Core/Host fixtures, and a live x64/x86
injection matrix are implemented. React domain calls now use one Tauri
`domain_request` command and event consumers use caller-owned Tauri channels. The Host
operation registry serves status and immutable-snapshot object/type queries; domains
not yet implemented return a stable capability error instead of reaching legacy code.
Object/type/package/instance collections now use exact full-path filters and
generation/query-bound cursor pages capped at 128 records; the Host reuses its snapshot
indexes instead of rescanning the snapshot or accepting legacy offset pagination.
The release DLL has no WinSock/WinHTTP/WinINet import or legacy HTTP marker according
to the transport cutover contract.

No Unreal Engine version is currently claimed as verified because the required target
fixtures have not yet been added. A successful build does not establish runtime safety.

## Repository layout

| Path | Purpose |
|---|---|
| `Dumper/` | Injected C++ Core, Unreal Engine reflection, generators, and retained non-release legacy sources |
| `frontend/` | React UI and Tauri Rust Host |
| `protocol/` | Versioned IPC contract and shared Rust framing crate |
| `tests/core-harness/` | Standalone C++ protocol, queue, backpressure, and shutdown tests |
| `tests/injection-fixture/` | Real x64/x86 target and injectable Ready/slow/reject DLL matrix |
| `tests/fake-core/` | Rust Fake Core for Host protocol tests |
| `tests/contracts/` | Issue register and legacy API drift checks |

## Prerequisites

- Windows x64
- Visual Studio with the Desktop development with C++ workload
- Node.js 22 and npm
- Rust stable MSVC with `rustfmt` and `clippy`

## Build and validation

Core DLL on the configured development machine:

```powershell
& 'D:\Program Files\Visual Studio 2026\MSBuild\Current\Bin\MSBuild.exe' `
  'D:\Projects\UExplorer\Dumper\UExplorerCore.vcxproj' `
  /p:Configuration=Release /p:Platform=x64 /m:1 /v:minimal
```

Frontend:

```powershell
Set-Location D:\Projects\UExplorer\frontend
npm ci
npm run lint
npm run test
npm run build
```

Rust crates:

```powershell
cargo test --manifest-path D:\Projects\UExplorer\frontend\src-tauri\Cargo.toml
cargo test --manifest-path D:\Projects\UExplorer\protocol\rust\Cargo.toml
cargo test --manifest-path D:\Projects\UExplorer\tests\fake-core\Cargo.toml
```

Core harness and contract checks:

```powershell
& 'D:\Program Files\Visual Studio 2026\MSBuild\Current\Bin\MSBuild.exe' `
  'D:\Projects\UExplorer\tests\core-harness\CoreHarness.vcxproj' `
  /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /m:1 /v:minimal

& 'D:\Projects\UExplorer\tests\core-harness\x64\Release\CoreHarness.exe' `
  'D:\Projects\UExplorer\protocol\v1\fixtures'

& 'D:\Program Files\Visual Studio 2026\MSBuild\Current\Bin\MSBuild.exe' `
  'D:\Projects\UExplorer\tests\injection-fixture\InjectionTarget.vcxproj' `
  /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /m:1 /v:minimal
& 'D:\Program Files\Visual Studio 2026\MSBuild\Current\Bin\MSBuild.exe' `
  'D:\Projects\UExplorer\tests\injection-fixture\InjectionTarget.vcxproj' `
  /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v145 /m:1 /v:minimal
& 'D:\Program Files\Visual Studio 2026\MSBuild\Current\Bin\MSBuild.exe' `
  'D:\Projects\UExplorer\tests\injection-fixture\InjectionCoreFixture.vcxproj' `
  /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /m:1 /v:minimal
& 'D:\Program Files\Visual Studio 2026\MSBuild\Current\Bin\MSBuild.exe' `
  'D:\Projects\UExplorer\tests\injection-fixture\InjectionCoreFixture.vcxproj' `
  /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v145 /m:1 /v:minimal
& 'D:\Program Files\Visual Studio 2026\MSBuild\Current\Bin\MSBuild.exe' `
  'D:\Projects\UExplorer\tests\injection-fixture\InjectionCoreFixture.vcxproj' `
  /p:Configuration=Slow /p:Platform=x64 /p:PlatformToolset=v145 /m:1 /v:minimal
& 'D:\Program Files\Visual Studio 2026\MSBuild\Current\Bin\MSBuild.exe' `
  'D:\Projects\UExplorer\tests\injection-fixture\InjectionCoreFixture.vcxproj' `
  /p:Configuration=Reject /p:Platform=x64 /p:PlatformToolset=v145 /m:1 /v:minimal

cargo test --manifest-path D:\Projects\UExplorer\frontend\src-tauri\Cargo.toml --features cross-language-fixture --test core_process_fixture
cargo test --manifest-path D:\Projects\UExplorer\frontend\src-tauri\Cargo.toml --features cross-language-fixture --test injection_process_fixture

& 'D:\Projects\UExplorer\tests\contracts\verify-issue-register.ps1'
& 'D:\Projects\UExplorer\tests\contracts\verify-api-v1.ps1'
& 'D:\Projects\UExplorer\tests\contracts\verify-injection-safety.ps1'
& 'D:\Projects\UExplorer\tests\contracts\verify-core-safety.ps1'
& 'D:\Projects\UExplorer\tests\contracts\verify-core-runtime.ps1'
& 'D:\Projects\UExplorer\tests\contracts\verify-host-snapshot.ps1'
& 'D:\Projects\UExplorer\tests\contracts\verify-host-session.ps1'
& 'D:\Projects\UExplorer\tests\contracts\verify-rpc-session.ps1'
& 'D:\Projects\UExplorer\tests\contracts\verify-named-pipe.ps1'
& 'D:\Projects\UExplorer\tests\contracts\verify-platform-safety.ps1'
& 'D:\Projects\UExplorer\tests\contracts\verify-offset-discovery.ps1'
& 'D:\Projects\UExplorer\tests\contracts\verify-transport-cutover.ps1' `
  -DllPath 'D:\Projects\UExplorer\Dumper\x64\Release\UExplorerCore.dll'
```

GitHub Actions runs the same quality gates with the Visual Studio 2022 `v143`
toolchain for portability. The local release command remains the required VS2026
`v145` validation path.
