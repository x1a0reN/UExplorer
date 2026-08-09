# UExplorer Desktop

This directory contains the React 19 UI and the Tauri 2 Rust Host.

The current UI still calls the legacy Core HTTP API. During R3-R4 it will be switched
atomically to Tauri commands and events backed by the Rust session manager and Named
Pipe RPC. New frontend code must not add direct Core HTTP, SSE, or WebSocket access.

R3 has started: `src-tauri/src/ipc/rpc_session.rs` implements the strict,
transport-independent RPC lifecycle and is exercised end-to-end against the shared
typed FakeCore. The actual Windows Named Pipe client, peer ACL/PID validation,
EventHub, and multi-PID SessionManager are not connected yet, so DLL load is still
not equivalent to Core ready.

## Commands

```powershell
npm ci
npm run lint
npm run test
npm run build
npm run tauri:dev
```

The canonical architecture, known limitations, and validation requirements are in the
repository root `README.md`, `REFACTOR_PLAN.md`, and `DESIGN.md`.
