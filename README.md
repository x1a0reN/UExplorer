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
operation registry serves status, immutable-snapshot object/type queries, exact type
details, stable-handle property reads, the bounded single-target `call.invoke`,
immutable current-world/level/actor queries, exact-handle stored/computed Actor
transform reads, strict Memory commands, bounded Watch commands, and capability-gated
Blueprint commands. The Dump domain now has a single-active immutable-snapshot worker
for bounded SDK/USMAP/Dumpspace/IDA artifacts. Domains without a published runtime producer, worker, or witness
return a stable capability error instead of reaching legacy code.
Baseline Core initialization is also separated from optional reflection/generator
activation: unknown property, FText, GWorld, or generator layouts cannot make the pipe
runtime pretend to be unsupported or execute ProcessEvent from the startup worker.
The Core now prepares a production reflection candidate from one complete immutable
object-snapshot generation. It locates an exact canonical fixture set, requires the
observed UProperty/FProperty system to match the immutable engine profile, and scans
checked live memory through the shared PostRender scheduler. `engine.reflection` opens
only after every required field and semantic witness publishes one immutable
`ReflectionLayout`; missing/ambiguous fixtures, dependency drift, or validation failure
remain explicit. Type metadata no longer depends on unrelated FText/container value
codecs: a matching `PropertyCodec` upgrades the same atomic snapshot later and opens
`engine.property_codec` independently. Range-valid legacy offsets or boolean probe
claims are insufficient.
The Core also contains a transport-neutral, SafeMemory-only `PropertyCodec` with
explicit `ok/empty/unsupported/unavailable/error` states, stable-handle references,
recursive budgets, coherent FString/array/sparse-container reads, and an atomic immutable
upgrade of the published `ReflectionRuntimeSnapshot`. Resolver success is rejected unless its handle is complete and matches the
resolver session/context and observed reference. Production reflection publication now
installs a layout-bound baseline codec and the type source freezes flat descriptors for
scalar, bool, FName, FString, FText, UObject, weak, and soft properties. The baseline
profile exposes only scalar, bool, FName, and UObject decoding; kinds that still need a
value-layout witness fail with `PROPERTY_CODEC_KIND_UNAVAILABLE` rather than being read
through guessed offsets.
Object/type/package/instance collections now use exact full-path filters and
generation/query-bound cursor pages capped at 128 records; the Host reuses its snapshot
indexes instead of rescanning the snapshot or accepting legacy offset pagination.
Production object snapshots keep records in segmented storage and validate each record
inside the existing capture budget. The final PostRender step therefore performs no
contiguous `reserve(N)` or second O(N) record scan. Replaced generations, rejected
publications, and failed working sets use fixed-capacity retirement slots and are
destroyed by the DLL worker thread; exhaustion is explicit backpressure, not synchronous
game-thread cleanup.
The Core also owns an immutable `TypeSnapshotStore` bound to one object generation and
one witnessed reflection layout. It requires complete Class/Struct/Enum and Function
coverage, freezes property descriptor graphs, stores direct members only, makes inherited
queries explicit, and validates exact CDO handles and bounded acyclic super chains. A
generic `TypeSnapshotCapture` now assembles strictly ordered metadata records under the
shared frame budget, revalidates the exact object/reflection `shared_ptr` dependencies,
and hands complete candidates to a worker-only `PublishReady` path. The Facade publication
entry is private to that owner and the handoff rejects the witnessed game thread. Failed candidates use
fixed-capacity worker reclamation with explicit backpressure. The production
`ObjectSnapshotTypeCandidateSource` freezes one exact object/reflection generation,
captures complete structural type/function coverage through stable handles and
SafeMemory, incrementally revalidates every live evidence record, and is attached by
Main to the same scheduler. It publishes witnessed super/CDO, direct property/parameter,
flat property descriptors, enum identity/backing candidates, and native-exec structure
while keeping enum entry tables and bytecode profiles unavailable until their own
immutable witnesses exist. A worker-only `TypeCommandService` now exposes exact-path
Class/Struct/Enum/Function detail, explicitly scoped direct/inherited member pages,
direct-child hierarchy pages, and CDO identity through Named Pipe. It reads only the
current immutable type snapshot: pages are capped at 128 records, cursors bind the
session/context, snapshot generation, and query fingerprint, serialized command data is capped at 4 MiB,
64-bit flags stay canonical hex strings, and enum int64 values stay decimal strings.
It does not enter the game thread, rescan live UE memory, fabricate CDO property values,
or make unavailable descriptors/enum layouts/bytecode profiles appear supported. No UE profile
is claimed until the target-process fixtures pass.
`ObjectPropertyCommandService` accepts only an exact ObjectSnapshot handle plus the
matching TypeSnapshot generation, declaring full path, exact property name, and fixed
array index. It rechecks the complete dependency set and object identity on the witnessed
game thread before decoding. Rust forwards the operation explicitly, while the React
client derives request identity from snapshot/type pages and never sends a bare index or
caller-supplied address.
`FunctionCallCommandService` accepts one exact target `ObjectHandle`, one exact
`FunctionHandle`, the matching TypeSnapshot generation, and arguments keyed by exact
reflected parameter names. An owned zeroed `ParamFrame` encodes trivial
bool/integer/float/double/UObject inputs plus descriptor-proven canonical FVector/FRotator
values with exact semantic fields and float/double width. The witnessed game-thread task revalidates the
target, function owner/path/signature, and every object argument immediately before
ProcessEvent, then decodes out/inout/return fields from the same owned frame. Static
calls use an explicit CDO handle; the protocol has no caller-controlled thread switch.
Descriptor-proven enum name/raw input and output decode plus an owned reverse-order
destructor journal are implemented. Production UEnum entry-table evidence is still
missing, so real enum descriptors remain unavailable; non-trivial FString/container/
  arbitrary-struct lifetimes and real UE round-trip evidence also remain unavailable.
`WorldSnapshotCapture` uses the exact immutable Object/Type generations to pre-index
Actor, Level, and ActorComponent candidates off-thread. Under the shared PostRender budget it resolves
the witnessed `GWorld` slot, follows the same typed-outer semantics used by
`AActor::GetLevel()` and `UActorComponent::GetOwner()`, and requires the reflected
`ULevel.OwningWorld` object field to equal the current world before publishing. It also
captures witnessed `AActor.RootComponent`, exact `UWorld.AuthorityGameMode/GameState`,
and the exact `OwningGameInstance -> LocalPlayers[0] -> PlayerController -> Pawn` chain.
The local-player path is enabled only when the same TypeSnapshot contains all required
object fields plus an exact `Array<Object /Script/Engine.LocalPlayer>` descriptor; it
does not search later array entries or fall back to a global class match. A second
game-thread pass revalidates the world, every stable handle, all Actor/Level/Component
ownership, root references, and the complete intermediate shortcut chain. Sorting, exact
counts, store validation, and publication run on the DLL worker between the bounded
game-thread phases rather than in the PostRender hot path. The worker-only
`WorldCommandService` exposes `world.inspect`, cursor-paged `world.levels`, filtered
`world.actors.list`, exact-handle `world.actor.get`, Actor-bound component pages, and
explicit-state shortcuts. If local-player metadata is missing, PlayerController/Pawn stay
explicitly unavailable. The production type source now derives canonical FVector and
FRotator descriptors only from exact `/Script/CoreUObject.Vector`/`Rotator` identity and
their witnessed `X/Y/Z` or `Pitch/Yaw/Roll` float/double fields.
`WorldTransformCommandService` exposes `world.actor.transform.get`: one owned game-thread
work freezes the exact World/Object/Type/codec generations, revalidates Actor and
RootComponent handles, reads `RelativeLocation`, `RelativeRotation`, `RelativeScale3D`,
and all three `bAbsolute*` flags from one bounded stable byte witness. When exact
`call.invoke` capability and matching Type/Object snapshot metadata are present, the same
work also revalidates and invokes `/Script/Engine.Actor.K2_GetActorLocation`,
`K2_GetActorRotation`, and `GetActorScale3D` through ProcessEvent using three owned
canonical return frames, then rechecks the root relation and stored live bytes before
publishing. Missing getter capability/metadata produces an explicit unavailable
`computed_transform`; it does not guess a layout or replace the stored result. WorldBrowser
separates float/double stored relative/absolute-world semantics from the reflected-getter
computed Actor world transform. The same UE 4.21-5.7 sources expose
`SetActorScale3D(FVector)` and `SetRelativeScale3D(FVector)` without complex outputs, so
those exact reflected functions now fit the canonical-struct `call.invoke` contract.
`world.actor.transform.update` accepts exactly one field per request and never presents
multiple ProcessEvent calls as atomic. Exact `SetActorScale3D`, `SetRelativeScale3D`, and
world `K2_SetActorRotation` signatures are admitted only after the same
session/context/Object/Type/World generations and Actor/RootComponent/function handles
are fixed and revalidated around ProcessEvent. Location and relative rotation remain
fail-closed because their reflected setters contain an `FHitResult&` output whose
lifetime is not witnessed. There is no direct memory-write or setter fallback, and a
post-invoke identity failure reports an unknown mutation state. This is implemented code, not a
target-runtime validation: `D:\Steam\steamapps\common\Wandering Sword` still has no
launchable game `.exe`, no real UE getter/ProcessEvent round-trip has run, and every engine
profile therefore remains `Not supported`.
PostRender now drives one `GameThreadFrameScheduler` rather than giving the object
snapshot producer an exclusive callback slot. The scheduler supports at most eight
clients, shares a 32-unit frame budget in four-unit round-robin quanta, stops further
dispatch after 2 ms, and quiet-drains each client independently. Frame clients are not
called after a pump-thread mismatch. Production object-snapshot, reflection, and type
capture use this scheduler; future watch collectors must do the same rather than add
another Hook or unbounded per-frame loop. Reflection capture temporarily pauses periodic
object-snapshot replacement so its exact generation dependency cannot drift before
validation/publication, then releases the retained plan on every terminal path.
World capture also blocks replacement only while its exact Object/Type generation is
being scanned and revalidated.
The release DLL has no WinSock/WinHTTP/WinINet import or legacy HTTP marker according
to the transport cutover contract.

The production reflection path is covered by complete synthetic UProperty and FProperty
memory graphs, including fail-closed profile mismatch, bounded field chains, no partial
publication, and shutdown ownership. A production type-source UProperty graph additionally
proves exact dependency sealing, complete structural coverage, worker publication, flat
property descriptors, one-level exact `Array<Object>` metadata, canonical UE4-float and
UE5-LWC-double FVector/FRotator descriptors, and mutation rejection during incremental
validation. Struct decoding snapshots and compares the whole bounded value around child
decoding so fields are not assembled from separate live reads. The Windows x64 property
profile uses the source-backed 16-byte ScriptArray header for FString and bounded array
decoding. No Unreal Engine version
is currently claimed as verified because the required target fixtures have not yet been
added. A successful build or synthetic fixture does not establish target runtime safety.

### R5 code checkpoint (2026-08-14)

- Memory now has strict bounded commands, preimage-backed verified writes, bounded
  rollback, protection-race detection, and no executable-page fallback.
- Watch is generation-bound and budgeted, with per-subscription pinned immutable
  bindings, bounded history/events, a visible snapshot/history UI, and explicit
  `watch.events.drain`. Independent bounded Watch/Hook push copies are drained by an
  owned worker outside the game thread/Hook callback and cross Named Pipe Event, Host
  EventHub, and caller-owned Tauri Channels; React applies typed increments locally.
- Blueprint raw bytecode capture is now wired through a generation-bound runtime source.
  It is advertised only when `UFunction::Script` passes the high-confidence multi-function
  witness gate and current Object/Type snapshots match; copied streams must retain a stable
  TArray header and end in `EX_EndOfScript`. `blueprint.decompile` is published only when
  the immutable runtime dependencies select an exact local-source profile for the active
  version marker, name/property model, outline-number mode and canonical math layout. The
  bounded parser fails closed at the first unreliable operand boundary; this remains
  code/synthetic evidence rather than a real-target opcode/operand witness.
- The call-batch coordinator is now wired through Main/Core/Host/schema/TypeScript. It owns
  one bounded serial job, total deadline, cancellation and retained per-item results; every
  item reuses the exact single-call preparation/game-thread/completion path. Snapshot drift
  fails closed, and no real UE batch round-trip has been run.
- Hook now has a production `ProcessEventHookOwner`: current TypeSnapshot Class/CDO
  evidence is live-validated on the game thread, unique vtable slots retain their own
  originals, and bounded fixed-metadata or scalar-parameter enter/exit events reach
  `HookEventCollector`. Scalar capture compiles an immutable plan from the exact
  generation-bound reflected function, admits only descriptor-proven trivial scalars,
  copies input/inout before ProcessEvent and output/inout/return afterward into a fixed
  512-byte callback buffer, caps per-phase name/type metadata at 8 KiB, and decodes only on the worker. Plan/frame/read/size failures
  stay explicit; unsupported lifecycle-bearing values do not downgrade to metadata.
  Capability follows exact current-generation coverage; restore/drain failure blocks
  unload. The generic `preencoded_payload` producer remains disabled, and no real UE Hook
  parameter or restore fixture has run.
- Dump start pins the exact immutable EngineContext, ObjectSnapshot, and TypeSnapshot in
  one owned single-active job. Terminal records release those large inputs while retaining
  their admitted scope and bounded events/results, so polling is not coupled to the current
  snapshot generation. The Host rejects caller output identities and injects a bounded
  UUID token; Core reserves only `%LOCALAPPDATA%\UExplorer\Dumps\<session>\<identity>`.
- `SnapshotDumpWorker` emits a directly includable snapshot C++ header with typed fields,
  enums, padding/asserts, function parameter structs, metadata, and ProcessEvent helpers
  (not a complete Dumper-7 runtime framework), an uncompressed USMAP v4 container, five Dumpspace JSON documents, or an
  IDA Python name script. Each path has a bounded structural consumer. Artifacts are written
  as `.partial`, checked by size plus SHA-256, then committed without replacement before
  `manifest.json` is validated and committed last with the pinned scope and artifact metadata. Cooperative
  cancellation/deadline and shutdown drain are wired, but Host restart persistence and
  real target artifact/consumer fixtures are not implemented.
- World mutation is a strict single-field reflected command for world/relative scale and
  world rotation. Location/relative rotation remain unavailable until `FHitResult` has a
  witnessed construction/destruction profile.
- No new target UE or Wandering Sword fixture was run. Every profile remains
  `Not supported`.

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
& 'D:\Projects\UExplorer\tests\contracts\verify-type-commands.ps1'
& 'D:\Projects\UExplorer\tests\contracts\verify-transport-cutover.ps1' `
  -DllPath 'D:\Projects\UExplorer\Dumper\x64\Release\UExplorerCore.dll'
```

GitHub Actions runs the same quality gates with the Visual Studio 2022 `v143`
toolchain for portability. The local release command remains the required VS2026
`v145` validation path.
