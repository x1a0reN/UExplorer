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
