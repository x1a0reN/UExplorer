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
  its serial-backed owner, full path, and signature fingerprint. An index, address,
  or short name alone is never an execution identity.
- Domain failures use a Response payload with `ok=false`; framing failures terminate
  the connection.

See `protocol.json`, `schema/payload.schema.json`, and `fixtures/` for the
machine-readable contract and golden data.
