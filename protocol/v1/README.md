# UExplorer IPC Protocol v1

This directory is the canonical contract between the injected Core and the Rust Host.
The desktop UI does not consume this protocol directly.

## Transport

- Windows Named Pipe: `\\.\pipe\UExplorer\v1\<target-pid>`
- Byte stream mode with an explicit 24-byte little-endian frame header
- UTF-8 JSON payloads
- Maximum payload: 8 MiB
- Compression: `none` only

## Header

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic `UEXP` |
| 4 | 2 | protocol major |
| 6 | 2 | protocol minor |
| 8 | 2 | frame kind |
| 10 | 2 | flags; must be zero in v1 |
| 12 | 4 | payload length |
| 16 | 8 | request ID |

All integer fields are unsigned little-endian. Unknown flags, invalid magic, unknown
frame kinds, a different major version, or payloads above 8 MiB are terminal protocol
errors. The connection is closed; no alternate protocol is attempted.

The frame kind selects the exact payload definition under `payload.schema.json/$defs`.
The schema root is therefore an `anyOf` shape catalogue, not a discriminator: v1
`Cancel` and `Shutdown` intentionally have the same JSON fields and are distinguished
by the header. Using root `oneOf` would reject either valid payload as ambiguous.

## Handshake

1. Host sends `Hello` with a nonzero request ID.
2. Core validates protocol major and target PID.
3. Core responds with `Welcome` using the same request ID.
4. Host validates target PID, session ID, limits, and capabilities before publishing
   the session as ready.

Requests are rejected before the handshake completes. Request `timeout_ms` is a
relative duration measured by Core from frame receipt using a monotonic clock; it is
not a wall-clock timestamp shared between processes.

`Welcome.limits` is a closed object. v1 requires all of these negotiated bounds:
`max_payload_bytes`, `pending_rpc_per_session`, `game_thread_tasks`,
`hook_event_ring`, `subscriber_events`, `dump_running`, and `max_timeout_ms`.
Missing, renamed, unknown, zero, or above-contract values reject the handshake; the
Host does not infer defaults.

## Host RPC lifecycle

The transport-independent Rust Host state machine is implemented in
`frontend/src-tauri/src/ipc/rpc_session.rs`:

```text
Created -> HelloSent -> Ready -> Closing -> Closed
                    \-> Failed (terminal protocol/envelope failure)
```

- The `Welcome` request ID must exactly match `Hello`; PID, selected version,
  session ID, required capabilities, and every negotiated limit are validated before
  `Ready` is observable.
- Every request has one monotonically allocated request ID, a monotonic deadline,
  and one pending-map entry. The negotiated pending and timeout limits are enforced
  before a frame is emitted.
- Explicit cancellation and deadline expiry retire the pending entry into a bounded
  late-response tombstone set. A correlated late terminal frame is consumed as late;
  an unknown correlation fails the session instead of being attached to another call.
- Ping/Pong validates request ID, session, nonce, and timestamp. Events require frame
  request ID zero, strictly increasing `seq`, and a nondecreasing `dropped_before`.
- Host-initiated Shutdown enters `Closing`; the acknowledgement must repeat its
  request ID, session ID, and reason before the session becomes `Closed`.
- Incoming payloads are checked against the negotiated limit. The streaming decoder
  applies that limit before buffering a body and buffers at most one incomplete frame
  even when one read contains many frames. The Host transport adapter must split
  reads into chunks no larger than 64 KiB.

`tests/fake-core` consumes the same strict Rust payload types. The Host/FakeCore test
executes Hello/Welcome, Request/Response, Ping/Pong, Cancel/late Response, and
Shutdown acknowledgement without a private fixture-only envelope.

The Core adapter is implemented by `Dumper/IPC/NamedPipeRpcServer.*`. It binds the
canonical target-PID name before game hooks are installed, applies a protected DACL
for the current user, rejects remote clients, verifies the client PID and SID, and
uses overlapped reads/writes plus owned listener/request/event-writer threads. Core
events enter a 1024-item DropOldest queue and are serialized/written only by the
joinable event writer. Each event has an internally assigned uint53 sequence, a
64 KiB serialized bound, and a cumulative `dropped_before` count. A blocked peer can
fill and drop this queue, but cannot block the producer or game thread. The Windows
Core harness uses a real NPFS client to verify the server PID, handshake, domain
request, queued game-thread cancellation, heartbeat, event envelope/drop diagnostics,
exact shutdown acknowledgement, and thread drain.

The Windows Host adapter is implemented by
`frontend/src-tauri/src/ipc/named_pipe_client.rs`. It opens only the canonical PID
pipe with overlapped I/O and identification-level SQOS, verifies
`GetNamedPipeServerProcessId` before Hello, and gives one joinable worker exclusive
ownership of `CoreRpcSession`. It bounds commands at 256, each read at 64 KiB, and
the transport event inbox at 1024; overflow increments a diagnostic counter instead
of blocking the reader. A Host deadline starts at command admission, and only its
remaining duration is sent to Core. Deadline and explicit cancellation emit Cancel
and complete each caller once. Every exit path cancels and settles pending overlapped
I/O before its stable buffer is released. Disconnect completes every pending call, while Shutdown
requires the exact protocol acknowledgement before the worker joins. Real NPFS tests
cover 1-byte response fragmentation, a Welcome followed immediately by Event,
concurrent Ping correlation, cancellation/deadline, event overflow without RPC
starvation, peer-PID rejection before Hello, partial Welcome header/payload disconnect,
Ready-idle and mid-request disconnect, malformed ready-session frames, missing Shutdown
acknowledgement, and a fresh connection to the same PID-scoped name.

The Host `EventHub` is session-scoped and retains at most 1024 events for exact replay.
It accepts at most 64 KiB of serialized data per event and shares the retained payload
with subscriber queues instead of cloning the JSON per consumer. It supports exact kind,
`watch_id`, and `hook_name` selectors, at most 64 subscribers, and a bounded
1..1024-event queue per subscriber. Producers use nonblocking enqueue;
a slow subscriber receives a cumulative `host_dropped_before` count without stalling
the Pipe reader or another subscriber. The Pipe inbox attaches its cumulative transport
drop snapshot to the next successfully queued event; EventHub combines that value with
subscriber-local drops as `host_dropped_before`. Event sequence plus Core and transport
drop counters must be monotonic, and every post-observation sequence gap must be
accounted for by one of those drop sources. Replay retains the transport drop snapshot;
history that is no longer available or cannot fit the requested subscriber capacity
fails explicitly rather than returning a partial result.

The Windows Host `SessionManager` supports up to 16 PID-keyed sessions with one explicit
active session. Each session owns immutable Welcome identity/capabilities, one
`CoreRpcClient`, EventHub, SnapshotCache, and joinable event forwarder. Snapshot refresh
pulls `objects.snapshot.page` using the remaining negotiated deadline, restarts a
generation-changing page chain at most three times, and atomically publishes only a
complete generation. Tauri owns the manager as application state. `inject_and_connect`
first acquires a PID-scoped RAII admission so concurrent callers cannot race duplicate
`LoadLibraryW` operations. It publishes success only after DLL load, post-load process
identity validation, the
PID-scoped Pipe handshake/Core Ready boundary, and a second process identity validation;
DLL already-loaded is not itself a Ready result. Target process start time is serialized
as a decimal string so JavaScript cannot truncate it. A post-connect identity mismatch
aborts only the existing transport and never sends Shutdown to a potentially reused PID.

Host events reach React through caller-owned `tauri::ipc::Channel<HostEvent>` bridges,
not global window events. `EventBridgeManager` owns at most 64 joinable workers, preserves
EventHub replay/filter/drop semantics, records channel delivery failures, and stops every
bridge for a PID before that session is disconnected. Worker shutdown remains exhaustive
even when one bridge panics. The frontend does not discover or retry a `runtime.ini`
endpoint; all remaining direct HTTP/SSE/WS desktop calls are legacy R4 cutover work.

`frontend/src-tauri/tests/core_process_fixture.rs` is the required real cross-language
gate. With feature `cross-language-fixture`, it launches the independently built C++
`CoreHarness.exe --host-session-fixture` process, connects `SessionManager` to that
process's actual PID-scoped pipe, validates Welcome and one Core Event, pulls a real
`EngineSnapshotStore` generation into the Host index, queries it, then requires exact
Shutdown and a zero C++ process exit. CI builds the C++ fixture before this test; a
missing executable or contract mismatch is a hard failure. Both C++ and Rust decoders
also run deterministic framing matrices over 256 mixed frames, eight chunk widths,
every truncation point, and 4096 mutated frames.

## Identity and errors

- Request, Response, Hello, Welcome, Cancel, Ping, Pong, and Shutdown use nonzero
  request IDs when correlation is required.
- Events use request ID zero and carry their own monotonic sequence.
- Every post-handshake command includes `session_id`.
- UObject operations use `objectHandle`: `session_id`, immutable context generation,
  object-array index, positive serial, canonical hex address, and class fingerprint.
  Core re-reads and compares every field at the game-thread execution point.
- UFunction operations use `functionHandle`, which binds a function object handle to
  its serial-backed owner, canonical identity path, and signature fingerprint. The
  canonical path is built from the complete outer chain and raw FName comparison
  index/number tokens (`Function fname:<hex>:<number>...`), so execution identity does
  not depend on an unsafe display-name decoder. Human-readable paths are separate
  metadata and are never accepted as the sole execution identity. An index, address,
  or short name alone is never an execution identity.
- Domain failures use a Response payload with `ok=false`; framing failures terminate
  the connection.

## Registered Core commands

The v1 command registry is explicit. Unknown operations return
`OPERATION_NOT_SUPPORTED`; they are never forwarded to a legacy route.

| Operation | Data | Execution |
|---|---|---|
| `status.inspect` | `{}` | Worker-safe immutable runtime snapshot |
| `status.engine` | `{}` | Worker-safe immutable engine/offset report |
| `status.health` | `{}` | Worker-safe liveness/readiness snapshot |
| `status.reconnect` | `{}` | Always `RECONNECT_DISABLED` until an exclusive generation transition exists |
| `objects.snapshot.page` | `{"cursor": null \| {"generation": uint53, "after_index": int32}, "limit": 1..128}` | Worker-safe immutable snapshot page |
| `objects.handle.issue` | `{"index": int32}` | PostRender game-thread identity re-read |
| `functions.handle.issue` | `{"index": int32}` | PostRender game-thread function/owner/path re-read |
| `objects.property.read` | `{"object": object_handle, "type_snapshot_generation": uint53, "declaring_type_path": full_path, "property_name": exact_name, "array_index": 0..1023}` | Game-thread read from the exact object/type/reflection generation |
| `call.invoke` | `{"target": object_handle, "function": function_handle, "type_snapshot_generation": uint53, "function_path": full_path, "arguments": {name: {kind, value}}}` | Single ProcessEvent call after exact game-thread identity/dependency revalidation |
| `memory.raw.read` / `memory.raw.write` | canonical non-null hex address plus 1..4096 bytes | Checked worker memory access; data writes reject executable pages and verify/rollback from a preimage |
| `memory.typed.read` / `memory.typed.write` | canonical address, strict scalar type, canonical string value for writes | Width/range checked scalar access without JavaScript-number narrowing |
| `memory.pointer_chain.resolve` | canonical base plus at most 64 signed decimal offsets | Checked pointer dereference/addition with explicit failed step |
| `watch.add/list/enable/remove/snapshot` | exact object handle/generations/property identity or bounded watch ID query | Generation-bound scheduler state; sampling runs under the shared frame budget |
| `watch.events.drain` | `{"limit": 1..32}` | Explicit bounded pull; this is not a Pipe Event/Tauri Channel push contract |
| `blueprint.bytecode` | exact FunctionHandle/path and context/Object/Type generations | Bounded Script capture; capability remains unavailable without a published capture witness |
| `blueprint.decompile` | bytecode identity plus explicit `profile_id` | Fail-closed bounded disassembly; capability remains unavailable without a matching immutable profile |
| `world.inspect` | `{}` | Worker-safe immutable current-world identity and exact Level/Actor counts |
| `world.levels` | `{"cursor": null \| world_cursor, "limit": 1..128}` | Worker-safe immutable Level page |
| `world.actors.list` | `{"cursor": null \| world_cursor, "limit": 1..128, "search": string \| null, "class_search": string \| null, "level_path": exact_path \| null}` | Worker-safe immutable Actor page with exact total matching |
| `world.shortcuts` | `{}` | Current-World GameMode/GameState references plus explicit unavailable LocalPlayer/Pawn states |
| `world.actor.get` | `{"actor": object_handle, "world_snapshot_generation": uint53}` | Exact immutable Actor/Level/root-component detail; transform state remains explicit |
| `world.actor.components` | `{"actor": object_handle, "world_snapshot_generation": uint53, "cursor": null \| world_cursor, "limit": 1..128}` | Cursor-paged immutable components owned by the exact Actor |
| `world.actor.transform.get` | `{"actor": object_handle, "world_snapshot_generation": uint53}` | One game-thread work separates stored RootComponent `Relative*`/`bAbsolute*` from optional computed Actor values obtained through exact reflected getters |
| `world.actor.transform.update` | exact session/context/Object/Type/World generations + Actor handle + one strict update union | One game-thread ProcessEvent mutation; world/relative scale and world rotation only, while FHitResult-backed variants fail closed |
| `types.classes.get` | `{"path": full_path}` | Worker-safe immutable class summary |
| `types.classes.fields` | `{"path": full_path, "scope": "direct" \| "include_inherited", "cursor": null \| type_cursor, "limit": 1..128}` | Worker-safe immutable field page |
| `types.classes.functions` | same member-page shape | Worker-safe immutable function page |
| `types.classes.hierarchy` | `{"path": full_path, "cursor": null \| type_cursor, "limit": 1..128}` | Complete bounded parent chain plus direct-child page |
| `types.classes.cdo` | `{"path": full_path}` | CDO state/handle only; never fabricated property values |
| `types.functions.get` | `{"path": full_function_path}` | Exact immutable function metadata and execution handle |
| `types.structs.get` | `{"path": full_path}` | Worker-safe immutable struct summary |
| `types.structs.fields` | same member-page shape | Worker-safe immutable struct field page |
| `types.enums.get` | `{"path": full_path}` | Worker-safe immutable enum summary/state |
| `types.enums.values` | `{"path": full_path, "cursor": null \| type_cursor, "limit": 1..128}` | Worker-safe immutable enum-value page |

Handle issue commands accept only an index as discovery input. Caller-supplied
addresses, serials, classes, owners, paths, or fingerprints are rejected rather than
trusted. The response contains the complete handle produced at the execution point.
`objects.handle.issue` requires `objects.handles`; `functions.handle.issue` requires
the stricter `functions.handles` capability. A valid object-handle profile therefore
cannot accidentally advertise function identity or function invocation support.

`objects.snapshot.page` returns records from one atomically published immutable
generation in ascending object-array index order. The first request uses a null
cursor. Every continuation cursor binds the generation and last returned index; if
Core has published a newer generation, it returns `SNAPSHOT_GENERATION_MISMATCH` and
the Host must discard the partial generation and restart with a null cursor. Core
does not search, filter, group, or cache these records. The Rust Host must consume a
complete generation before publishing its type, path, package, address, and search
indexes. Snapshot pages never enter the game-thread queue and are capped at 128
records; snapshot-wide `source_object_count`, `record_count`, and `skipped_slots` are
exact rather than page-relative estimates.

`objects.property.read` never accepts a bare index, caller-supplied address, short
type name, or inferred owner. The object handle must be an exact member of the
current immutable ObjectSnapshot; the declaring type and property must resolve in
the matching TypeSnapshot generation. Execution revalidates the handle and all
snapshot/codec dependencies on the witnessed game thread before decoding.

`call.invoke` never accepts a target index, function short name, caller address, or
`use_game_thread` option. Input and inout parameters are mandatory; output and return
parameters cannot be supplied. Each argument envelope repeats the exact reflected kind.
The codec accepts bool, signed/unsigned integers as canonical decimal strings, finite
float/double strings, null or stable UObject handles, exact canonical FVector/FRotator,
and descriptor-proven enum name/raw selections. Core owns the exact reflected parameter
frame and a reverse-order destructor journal. Production UEnum entry-table evidence is
not yet available, and non-trivial UE value lifetimes and batch commands are not silently
approximated.

Memory writes retain a bounded preimage, verify the committed bytes, and report rollback
and protection-race outcomes. There is no executable/code-write fallback. Watch events
are currently consumed only through `watch.events.drain`; the existing transport Event
frame does not imply a Watch producer. Blueprint request schemas are registered, but Main
publishes neither a Script layout witness nor a bytecode profile, so capability gating
fails before capture/decompile.

Hook and Dump command layers exist only behind unavailable capabilities at this
checkpoint. A transport-neutral batch command/worker boundary exists, but no configured
Core exact-call adapter or cross-layer route does; `call.batch` therefore advertises the
independent `CALL_BATCH_ADAPTER_NOT_READY` capability reason. Legacy `call.static` is retired;
static invocation uses `call.invoke` with an explicit CDO handle. Core contains bounded
collector/coordinator primitives, but no ProcessEvent producer, generator worker, or
batch worker bridge, so none may select a legacy implementation.

World commands read only a `WorldSnapshot` whose session, context, ObjectSnapshot, and
TypeSnapshot generations still match the active immutable dependencies. A world cursor
contains the WorldSnapshot generation, the last returned source ordinal, and a 16-hex
query fingerprint derived from session/context/Object/Type/World generation, operation,
and filters. It cannot cross a generation or filter, point at an unmatched Actor, or
represent a terminal page for which the service never emitted a continuation. Actor
queries return exact `matched` after scanning the immutable snapshot; each response is
capped at 128 records and 4 MiB. Actor details and component pages require both the exact
Actor handle and WorldSnapshot generation. Components are associated only through the
witnessed Actor typed-outer relation, and `RootComponent` is read through its reflected
object field. GameMode and GameState shortcuts are read from the exact current UWorld and
must resolve back to an Actor in that World. PlayerController/Pawn are admitted only
through the witnessed `UGameInstance.LocalPlayers[0]` chain and otherwise stay explicitly
unavailable. Transform reads use exact canonical math descriptors and keep stored values
separate from reflected-getter computed Actor values. `world.mutate` admits only the
strict single-field reflected setter service. Location and relative rotation return
stable lifetime-unavailable errors until an exact `FHitResult` construction/destruction
profile exists; there is no raw memory or alternate setter fallback.

The shared Rust types live in `protocol/rust`; the Host must deserialize pages with
unknown-field rejection. `frontend/src-tauri/src/session/snapshot_cache.rs` validates
the complete cursor chain and snapshot-wide metadata before atomically publishing a
generation. A rejected or incomplete generation never replaces the current cache.

Type commands read only the atomically published `TypeSnapshot` that matches the
active session, context generation, object-snapshot generation, and reflection-layout
fingerprint. Detail commands require an exact full path; short names are discovery
labels and are rejected as identity. Collection cursors contain
`generation`, `after_ordinal`, and a 16-hex-digit query fingerprint derived from the
session/context/object dependency, operation, path, and scope. A cursor cannot cross a
session or type generation and cannot be reused for another query. Pages contain at
most 128 records and the serialized command data is
capped at 4 MiB. Function records cap serialized parameters at 128 and fail explicitly
instead of truncating. Property/function flags are canonical 64-bit hex strings;
signed enum values are decimal strings so the Host and React never narrow them through
JavaScript Number. Unsupported and unavailable properties, functions, enums, and CDOs
retain their state and reason. `types.classes.cdo` deliberately does not return a fake
empty property list before the property codec commands exist.

See `protocol.json`, `schema/payload.schema.json`, and `fixtures/` for the
machine-readable contract and golden data.
