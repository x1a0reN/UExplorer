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
array, map, and set decoding. It also supplies the exact FProperty field set with
per-field stable scalar/pointer/FName witnesses, including exact int32 UStruct property
size and minimum alignment, then verifies generation/fingerprint binding. Layout publication and
the fingerprint-matched codec upgrade replace the same immutable reflection runtime
snapshot atomically, while reflection and property-codec capabilities remain independent. This proves
validator, codec-boundary, and result-state behavior only; it is not evidence that the
same offsets/layouts apply to any engine row above. Production remains unavailable until
a target-specific candidate/witness source passes the same boundary.

The generic harness also publishes a synthetic immutable type snapshot only after exact
Class/Struct/Enum and Function coverage matches one object-snapshot generation. It proves
deep-frozen descriptor graphs, bounded property/parameter ranges, explicit direct versus
inherited member order, exact CDO handle/class matching, hierarchy cycle/depth rejection,
and stale-generation capability closure. It does not provide a production reflection or
type capture source and therefore does not change any support row.

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
object layouts, GC behavior, ProcessEvent, Watch/Hook producers, generator semantics,
or clean unload in a supported engine, so every engine row above remains `Not supported`.
