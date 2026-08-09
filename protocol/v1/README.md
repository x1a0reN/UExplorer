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

## Handshake

1. Host sends `Hello` with a nonzero request ID.
2. Core validates protocol major and target PID.
3. Core responds with `Welcome` using the same request ID.
4. Host validates target PID, session ID, limits, and capabilities before publishing
   the session as ready.

Requests are rejected before the handshake completes. Request `timeout_ms` is a
relative duration measured by Core from frame receipt using a monotonic clock; it is
not a wall-clock timestamp shared between processes.

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

The shared Rust types live in `protocol/rust`; the Host must deserialize pages with
unknown-field rejection. `frontend/src-tauri/src/session/snapshot_cache.rs` validates
the complete cursor chain and snapshot-wide metadata before atomically publishing a
generation. A rejected or incomplete generation never replaces the current cache.

See `protocol.json`, `schema/payload.schema.json`, and `fixtures/` for the
machine-readable contract and golden data.
