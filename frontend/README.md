# UExplorer Desktop

This directory contains the React 19 UI and the Tauri 2 Rust Host.

R4 transport cutover is complete. React domain calls enter
`src/api/client.ts -> Tauri domain_request -> Rust DomainService`; session events use
caller-owned Tauri channels. The Host then reaches the selected Core only through its
PID-scoped Windows Named Pipe session. Frontend code must not add direct Core HTTP,
SSE, WebSocket, endpoint-file, port, or token access.

The Windows Host owns strict peer PID/session validation, bounded request/event queues,
deadline/cancel behavior, EventHub, immutable snapshot indexes, multi-PID
SessionManager, injection-to-Core-Ready gating, and joinable shutdown. Status and basic
snapshot-backed Object/Type queries are currently implemented. Other domains return
explicit capability errors until their R5 services and fixtures exist; the UI must not
replace them with placeholders or transport fallback.

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
