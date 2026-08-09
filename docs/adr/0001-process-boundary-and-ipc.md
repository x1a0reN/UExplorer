# ADR-0001: Process Boundary and IPC

- Status: Accepted
- Date: 2026-08-09
- Decision owners: Core Runtime, Host, Frontend
- Related issues: NET-001, UI-001, ENG-008

## Context

The injected DLL currently owns Unreal Engine access, HTTP parsing, authentication,
SSE/WebSocket connections, event serialization, and client worker threads. A slow or
malformed network client can therefore affect the target process and DLL unload safety.
The React application also talks directly to the DLL and duplicates session recovery,
polling, and transport state.

## Decision

The desktop runtime has one production path:

```text
React -> Tauri invoke/event -> Rust Host -> Named Pipe RPC -> Core DLL
```

- Core DLL owns only target-process capabilities and bounded execution queues.
- Rust Host owns injection, process/session identity, IPC, jobs, caching, event fanout,
  configuration, and diagnostics.
- React never opens a Core socket and never discovers sessions through `runtime.ini`.
- Optional external HTTP/WebSocket access is implemented only by the Rust Host and
  calls the same domain services used by Tauri commands.
- Protocol major versions must match exactly. There is no HTTP fallback, alternate
  pipe fallback, guessed transport, or dual production stack.

The v1 pipe name is `\\.\pipe\UExplorer\v1\<pid>`. The Host must verify that the pipe
server PID matches the selected target process. The pipe ACL permits only the current
user and SYSTEM, and remote clients are rejected.

## Consequences

- Network and UI failures no longer execute inside the target process.
- Core shutdown can be proven from a finite set of owned threads, hooks, queues, and
  pipe handles.
- The protocol becomes a versioned product surface with golden frames and contract
  tests.
- Transport cutover must be atomic. Old Core HTTP sources may remain archived during
  development, but they cannot be compiled into or reachable from a release build
  after R4.
- Features without a verified Core capability return `unavailable`; they do not use
  a less safe execution path.

## Rejected alternatives

### Keep HTTP in the injected DLL

Rejected because connection lifetime, parsing, and backpressure would remain part of
the target process failure domain.

### Support HTTP and Named Pipe simultaneously

Rejected because two active transports create divergent authentication, lifecycle,
error, and event semantics and violate the no-fallback requirement.

### Move all Unreal Engine logic into Rust

Rejected because reflection, offset discovery, ProcessEvent, and hooks must execute in
the target address space. The boundary should be narrow, not artificial.
