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
   serial, address, class/owner, path, and signature validation.
4. Representative scalar, string, object, struct, array, map, set, delegate, soft
   object, FVector, and FRotator properties.
5. Native and Blueprint functions with input, output, in-out, and return parameters.
6. A deterministic Watch mutation and bounded Hook event source.
7. Expected C++ SDK, USMAP, Dumpspace, and IDA artifact validation results.
8. Connect, disconnect, reconnect, cancellation, and clean unload traces.

Until a fixture exists, the corresponding profile remains `Not supported`; offset or
layout heuristics must not upgrade that status.
