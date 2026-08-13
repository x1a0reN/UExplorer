# Supported Runtime Matrix

Support is evidence-based. A version is supported only after its fixture suite and a
target-process run pass. Source compatibility or a successful build is not sufficient.

The Core build target is Windows x64 only. x86/Win32 is intentionally unsupported and
fails at compile time; there is no 32-bit runtime capability or fallback path.

| Engine profile | Fixture | Current status | Enabled capabilities | Required evidence |
|---|---|---|---|---|
| UE 4.26 x64 | Missing | Not supported | None claimed | Object/name arrays, offsets, ProcessEvent, bytecode, property codecs, SDK/USMAP consumer, clean unload |
| UE 4.27 x64 | Missing | Not supported | None claimed | Object/name arrays, offsets, ProcessEvent, bytecode, property codecs, SDK/USMAP consumer, clean unload |
| UE 5.x x64 with LWC | Missing | Not supported | None claimed | FName/ObjectArray profile, LWC FVector/FRotator codecs, ProcessEvent, bytecode, SDK/USMAP consumer, clean unload |

## Local evidence inventory

These assets are available for development, but none changes the support rows above:

- The current R5 code checkpoint adds strict Memory commands, a bounded Watch scheduler
  with pinned immutable subscription bindings, explicit pull drain, and an independent
  bounded push copy drained outside the game thread through Named Pipe/Host/Tauri,
  descriptor-proven
  enum codecs and an owned destructor journal, generation-bound raw Blueprint Script
  capture plus a runtime-gated local-source profile catalog and bounded disassembly,
  generation-covered ProcessEvent Hook
  production ownership plus immutable-plan bounded trivial-scalar parameter capture,
  a single-active immutable-snapshot Dump worker, and
  an end-to-end bounded call-batch command path that reuses the exact single-call adapter.
  Dump start pins exact EngineContext/ObjectSnapshot/TypeSnapshot pointers, uses a Host-owned
  UUID identity under `%LOCALAPPDATA%\UExplorer\Dumps`, and emits bounded SDK/USMAP/
  Dumpspace/IDA artifacts through structural consumers, no-replace commit, size/SHA-256
  verification, and a final manifest. The SDK artifact is a directly includable typed snapshot
  header with fields, enums, parameter structs, padding/asserts, metadata, and ProcessEvent helpers;
  it is not a complete Dumper-7 runtime framework. It has no target-process evidence for any of them. Production
  UEnum entry-table and real-target Blueprint profile/opcode/operand witnesses, Hook
  non-trivial parameter lifecycle/caller/filtering and real parameter-frame evidence,
  Watch/Hook reconnect/slow-consumer/target restore/rate fixtures, Host
  dump-job reload persistence, real target/external
  semantic consumers for all four artifact formats, and a real batch ProcessEvent fixture are still missing, so no
  profile support claim follows from these source changes.

- `D:\Projects\UnrealEngine` contains source trees for UE 4.21.2, 4.24.3,
  4.25.4, 4.26.2, 4.27.2, 5.0.3, 5.1.1, 5.2.1, 5.3.2, 5.4.4,
  5.6.1, and 5.7.4. Source is used to derive candidate layouts, version transitions,
  lifecycle semantics, and fixture expectations; runtime witnesses still decide whether
  a candidate applies to a target binary.
- Representative UE 4.21/4.27/5.0/5.7 source confirms that the default-allocator
  `FScriptArray` consists of one data pointer followed by `int32 ArrayNum` and
  `int32 ArrayMax`, and that `UGameInstance::GetFirstGamePlayer()` selects
  `LocalPlayers[0]`. The corresponding World/GameInstance/Player/Controller fields remain
  reflected across those versions. This supports the Windows x64 ScriptArray and local-player
  candidates implemented in R5; it does not prove a Shipping binary uses those candidates.
- UE 4.21, 4.24-4.27, UE 5.0-5.4, 5.6, and 5.7 `Script.h` source also proves that opcode assignments are
  not one universal table: for example UE4 uses raw `0x38` for `EX_PrimitiveCast`, while
  UE5 uses `0x37` for `EX_DoubleConst`. The parser therefore keeps semantic token IDs
  separate from raw mappings. The production catalog selects only an inventoried marker
  family and additionally requires matching runtime name storage, FProperty/name-number
  mode, LWC flag, and canonical Vector/Rotator snapshot layouts; unsupported families fail
  closed. Those gates select a source candidate but do not prove a customized Shipping
  binary retained the catalogued `CodeSkipSizeType`, opcode table, or operand serializer.
- UE 4.21/4.27 `Vector.h` and `Rotator.h` define `X/Y/Z` and `Pitch/Yaw/Roll`
  as three packed `float` fields. UE5 LWC math declarations use the same semantic field
  names, while 5.4/5.7 `MathFwd.h` aliases the default FVector/FRotator to the `double`
  template instantiations. `SceneComponent.h` keeps reflected `RelativeLocation`,
  `RelativeRotation`, and `RelativeScale3D` members across the sampled UE4/UE5 trees.
  This evidence defines exact candidate identities and fixture expectations only.
- `SceneComponent.h` across the 4.21-5.7 source inventory preserves reflected
  `RelativeLocation`, `RelativeRotation`, `RelativeScale3D`, and the three `bAbsolute*`
  flags. It also preserves `K2_SetRelativeLocation(FVector, bool, FHitResult&, bool)` and
  `K2_SetRelativeRotation(FRotator, bool, FHitResult&, bool)`, while
  `SetRelativeScale3D(FVector)` has no `FHitResult` output. This is useful for selecting
  semantic properties and planning the owned ProcessEvent frame lifecycle, but it is not
  permission to assume offsets or zero-initialize an uncaptured Shipping `FHitResult`.
- `Actor.h`/`Actor.cpp` across the same source inventory preserves reflected
  `K2_GetActorLocation() -> FVector`, `K2_GetActorRotation() -> FRotator`,
  `GetActorScale3D() -> FVector`, and `SetActorScale3D(FVector)` without complex output
  parameters. R5 admits only descriptor-proven canonical FVector/FRotator slots in owned
  ProcessEvent frames; the three getters are now wired into the same owned transform-read
  work and the scale setter is code-reachable through exact `call.invoke`. This is source
  and implementation evidence only, not a supported target profile or a successful
  Shipping ProcessEvent round-trip.
- The R5 call implementation now owns direct FString input buffers, uses reflected
  `KismetTextLibrary` conversions for FText on the game thread, and recursively encodes
  descriptor-backed value structs composed of scalar/object/enum/nested-struct fields.
  Complete cleanup for UE-owned text/string return allocations, container/reference
  struct fields, and target ProcessEvent round-trips remain unverified; this does not
  upgrade any engine profile.
- `D:\Steam\steamapps\common\Wandering Sword` is the designated real-game fixture.
  Earlier passive artifacts are consistent with an x64 UE4/PhysX Shipping build in the
  UE 4.26 family, but the 2026-08-11 inventory contains only the IDA
  `JH-Win64-Shipping.exe.i64` database, a local `version.dll`, prior injection logs, and
  third-party trainer files under `Wandering_Sword\Binaries\Win64`; no launchable game
  executable is present. The official executable must be restored before a controlled
  baseline can establish engine identity, loaded modules, offsets, GC behavior, or clean
  unload. Existing derived artifacts cannot change the UE 4.26 support row.

## Fixture requirements

Each fixture must record:

1. Exact engine version and executable identity.
2. Expected GObjects, GNames/FNamePool, ProcessEvent, and critical offsets; GWorld/GEngine
   must also prove one unique writable data-slot candidate with stable typed object-array witnesses.
3. Stable object/function handles including session, context generation, index,
   serial, address, class/owner, path, and signature validation; complete snapshot
   paging must preserve one generation under representative GC churn.
4. Representative scalar, string, object, struct, array, map, set, delegate, soft
   object, FVector, and FRotator properties.
5. Native and Blueprint functions with input, output, in-out, and return parameters.
6. A deterministic Watch mutation and bounded Hook event source.
7. Expected C++ SDK, USMAP, Dumpspace, and IDA artifact validation results.
8. Connect, disconnect, reconnect, cancellation, and clean unload traces.

Until a fixture exists, the corresponding profile remains `Not supported`; offset or
layout heuristics must not upgrade that status.

Baseline startup discovers only the runtime fields required for object identity,
ProcessEvent, and the PostRender pump. Reflection, property/FText, GWorld/GEngine, and
Generator indexing are separate capabilities. Failure to validate one of those optional
layouts must leave that domain unavailable rather than failing or silently guessing the
transport/runtime profile.

PostRender domain work is accepted only on the executor's witnessed pump thread and is
routed through the bounded `GameThreadFrameScheduler` (8 clients, 32 aggregate work
units, 4-unit quantum, 2 ms dispatch deadline). This scheduling contract is covered by
synthetic fixtures. Object snapshot record validation and storage are also incremental;
old/rejected/failed generations are reclaimed off-frame under fixed backpressure.
Minimized/loading behavior, per-record worst-case time, and target-scale memory/tail
latency remain unverified until the R7 UE fixtures exist.

The generic Core harness now exercises a synthetic, explicitly validated x64 property
profile for scalar, FName/FString/FText, object/weak/soft reference, enum, struct,
array, map, and set decoding. It also feeds complete UProperty and FProperty live-memory
graphs through the production `ObjectSnapshotReflectionCandidateSource`, including exact
int32 UStruct property size/minimum alignment and per-field stable scalar/pointer/FName
witnesses. The candidate enters the owned `ReflectionLayoutCapture` one field at a time;
the fixtures verify immutable-profile mismatch, bounded offset/field-chain work, no
partial visibility, dependency revalidation, source-contract failure, same-thread
validation/publication, prepared-plan release, and drain. Layout publication and
the fingerprint-matched codec upgrade replace the same immutable reflection runtime
snapshot atomically, while reflection and property-codec capabilities remain independent. This proves
validator, production-source boundary, and result-state behavior only; it is not evidence
that the same offsets/layouts apply to any engine row above. The production source is
attached to the runtime scheduler and fails closed when its exact fixtures or witnesses
do not match, but no engine row can become supported until the same path passes that
row's target-process fixture.

The generic harness also feeds complete metadata through both the generic
`TypeSnapshotCapture` record fixture and the production
`ObjectSnapshotTypeCandidateSource`. The production source freezes one exact
object/reflection generation, walks witnessed UProperty field/function chains only
through stable handles and `SafeMemory`, emits complete structural type/function coverage,
and incrementally re-reads every evidence record before sealing. Main attaches that source
to the shared scheduler and performs worker-only publication after quiet detach. The
fixtures prove per-unit capture/validation, no partial visibility, exact dependency
identity, mutation rejection, fixed-capacity retirement/backpressure, deep-frozen generic
  descriptor graphs, bounded member ranges, direct versus inherited ordering, exact
  CDO/owner matching, and hierarchy guards. The production stream now freezes flat
  descriptors for scalar/bool/FName/FString/FText/UObject/Weak/Soft properties, one-level
  arrays whose element has a supported flat descriptor, and canonical FVector/FRotator
  struct descriptors when exact type path, semantic field names, scalar kind, packed offsets,
  size, and alignment all agree. Exact `Array<Object>` metadata also
  requires the element class full path from the same ObjectSnapshot. The Windows x64 profile
  opens scalar/bool/FName/FString/UObject, bounded array decoding, and the canonical math
  structs above; FText, Weak/Soft, arbitrary/deeper Struct/container descriptors, Map/Set,
  UEnum entry tables, and Blueprint decompile profiles
  are still explicitly unavailable without their own witnessed layouts. The same
harness exercises the worker-only immutable `TypeCommandService` for exact-path
Class/Struct/Enum/Function detail, explicit member scope, hierarchy/CDO metadata,
session/context/generation/query-bound pagination, 128-record page limits, a 4 MiB command-data ceiling,
hex 64-bit flags, decimal-string enum int64 values, and deterministic unavailable states.
The exact `objects.property.read` path additionally binds an ObjectSnapshot handle,
TypeSnapshot generation, declaring full path, property name, and array index, then
revalidates the dependencies and handle on the game thread before SafeMemory decoding.
Struct decoding reads a bounded whole-value witness twice, decodes children from that owned
snapshot, and compares the live bytes again before returning. The dedicated
`world.actor.transform.get` path now resolves the six exact SceneComponent properties and
reads them in one owned game-thread work from a shared bounded byte witness. When exact
reflected Actor getter metadata and ProcessEvent capability are present, that work also
validates and invokes the location/rotation/scale getters with three owned canonical
return frames before the final RootComponent and byte-range comparison. The response
keeps stored relative fields/`bAbsolute*` semantics separate from the reflected-getter
computed Actor world transform; unavailable getter metadata is reported explicitly.
The code path now admits one exact reflected mutation at a time for world/relative scale
or world rotation, with identity revalidation around ProcessEvent. FHitResult-backed
location/relative-rotation setters and every real UE target round-trip remain pending.
Rust/schema fixtures and React builds cover their side of this contract. The real
cross-language process fixture does not yet publish a production TypeSnapshot, and no
target-process reflection/type run exists, so this does not change any support row.

## Generic boundary evidence (not an engine support claim)

The R4 desktop transport boundary is verified independently of an Unreal profile:

- the release Core project contains only the PID-scoped Named Pipe transport and its
  binary has no WinSock/WinHTTP/WinINet import or legacy HTTP marker;
- the real C++ Core/Rust Host process fixture reaches Core status and immutable
  snapshot queries through `DomainService`, follows two generation-bound cursor pages,
  and verifies explicit unavailable and unknown-operation errors;
- Host fixtures prove exact full/class/package path filters, query-fingerprint cursor
  rejection, exact collection totals, and the strict 128-record response ceiling;
- the injection fixture covers real x64/x86 process identity, load, timeout, duplicate
  load, rejection, Pipe handshake, and Core Ready stages.

These fixtures prove the process and transport boundary only. They do not validate UE
object layouts, GC behavior, ProcessEvent, Watch/Hook push target behavior, generator semantics,
or clean unload in a supported engine, so every engine row above remains `Not supported`.
