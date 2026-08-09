$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path

function Read-ProjectFile([string]$relativePath) {
    return Get-Content -LiteralPath (Join-Path $root $relativePath) -Raw -Encoding UTF8
}

function Assert-Contains([string]$text, [string]$token, [string]$message) {
    if (-not $text.Contains($token)) {
        throw "$message Missing token: $token"
    }
}

function Assert-NotContains([string]$text, [string]$token, [string]$message) {
    if ($text.Contains($token)) {
        throw "$message Forbidden token: $token"
    }
}

$protocol = Read-ProjectFile 'protocol\rust\src\lib.rs'
$rpcSession = Read-ProjectFile 'frontend\src-tauri\src\ipc\rpc_session.rs'
$hostCargo = Read-ProjectFile 'frontend\src-tauri\Cargo.toml'
$fakeCore = Read-ProjectFile 'tests\fake-core\src\lib.rs'
$schema = Read-ProjectFile 'protocol\v1\schema\payload.schema.json' | ConvertFrom-Json
$protocolManifest = Read-ProjectFile 'protocol\v1\protocol.json' | ConvertFrom-Json
$welcome = Read-ProjectFile 'protocol\v1\fixtures\welcome.json' | ConvertFrom-Json
$heartbeat = Read-ProjectFile 'protocol\v1\fixtures\heartbeat.json' | ConvertFrom-Json
$shutdown = Read-ProjectFile 'protocol\v1\fixtures\shutdown.json' | ConvertFrom-Json

foreach ($token in @(
        'pub struct HelloPayload', 'pub struct WelcomePayload', 'pub struct ProtocolLimits',
        'pub struct RequestPayload', 'pub struct ResponsePayload', 'pub struct EventPayload',
        'pub struct CancelPayload', 'pub struct HeartbeatPayload', 'pub struct ShutdownPayload',
        'serde(deny_unknown_fields)', 'Consume at most one frame',
        'set_payload_limit',
        'large_coalesced_input_keeps_only_one_partial_frame_buffered',
        'negotiated_decoder_limit_rejects_the_header_before_buffering_its_body',
        'rpc_golden_payloads_use_the_shared_strict_types')) {
    Assert-Contains $protocol $token 'Canonical typed RPC/framing contract regressed.'
}

foreach ($token in @(
        'pub enum RpcSessionState', 'Created', 'HelloSent', 'Ready', 'Closing', 'Closed', 'Failed',
        'pub struct CoreRpcSession', 'start_handshake', 'start_request', 'cancel_request',
        'expire_requests', 'start_ping', 'start_shutdown', 'handshake_request_id',
        'pending: BTreeMap', 'retired: VecDeque', 'RETIRED_REQUEST_TTL_US',
        'transport.named_pipe', 'PeerPidMismatch', 'SessionMismatch',
        'pending_rpc_per_session', 'max_payload_bytes', 'last_event_seq',
        'MAX_RECEIVE_CHUNK_BYTES',
        'last_dropped_before', 'LateResponse', 'LatePong',
        'Shutdown acknowledgement does not match the request',
        'invalid_welcome_identity_capability_and_limits_are_terminal',
        'strict_request_response_correlation_and_envelope_are_enforced',
        'negotiated_payload_and_minor_version_are_enforced_after_handshake',
        'fake_core_drives_the_complete_host_rpc_lifecycle_contract')) {
    Assert-Contains $rpcSession $token 'Host RPC session state/correlation/safety contract regressed.'
}
Assert-Contains $hostCargo 'uexplorer-fake-core = { path = "../../tests/fake-core" }' 'Host lost its executable FakeCore contract dependency.'
foreach ($token in @('TcpStream', 'reqwest', 'http://', 'runtime.ini', 'uexplorer-dev')) {
    Assert-NotContains $rpcSession $token 'Host RPC state machine gained a legacy transport or token dependency.'
}

foreach ($token in @(
        'HelloPayload', 'WelcomePayload', 'RequestPayload', 'ResponsePayload',
        'HeartbeatPayload', 'CancelPayload', 'ShutdownPayload',
        'transport.named_pipe', 'pending_rpc_per_session', 'status.inspect',
        'REQUEST_CANCELLED', 'OPERATION_UNSUPPORTED')) {
    Assert-Contains $fakeCore $token 'FakeCore drifted from the canonical RPC contract.'
}
foreach ($token in @('struct Hello', 'struct Welcome', 'struct Request', 'status.get', '"pending_rpc"')) {
    Assert-NotContains $fakeCore $token 'FakeCore reintroduced a private or legacy protocol shape.'
}

$limits = $schema.'$defs'.protocolLimits
if (-not $schema.anyOf -or $schema.oneOf) {
    throw 'Schema root must be an anyOf catalogue because the frame header, not JSON shape, discriminates Cancel from Shutdown.'
}
if ($limits.additionalProperties -ne $false -or
        $limits.properties.max_payload_bytes.maximum -ne 8388608 -or
        $limits.properties.pending_rpc_per_session.maximum -ne 256 -or
        $limits.properties.game_thread_tasks.maximum -ne 128 -or
        $limits.properties.hook_event_ring.maximum -ne 8192 -or
        $limits.properties.subscriber_events.maximum -ne 1024 -or
        $limits.properties.dump_running.const -ne 1 -or
        $limits.properties.max_timeout_ms.maximum -ne 120000) {
    throw 'Schema RPC limits drifted from the bounded v1 contract.'
}

foreach ($required in @('max_payload_bytes', 'pending_rpc_per_session', 'game_thread_tasks',
        'hook_event_ring', 'subscriber_events', 'dump_running', 'max_timeout_ms')) {
    if ($limits.required -notcontains $required) {
        throw "Schema protocolLimits no longer requires $required."
    }
}

if ($protocolManifest.limits.pending_rpc_per_session -ne 256 -or
        $welcome.limits.pending_rpc_per_session -ne 256 -or
        $welcome.target_pid -ne 4242 -or
        $welcome.capabilities.'engine.core' -ne $true -or
        $welcome.capabilities.'transport.named_pipe' -ne $true -or
        $heartbeat.nonce -le 0 -or
        $heartbeat.session_id -ne $welcome.session_id -or
        [string]::IsNullOrWhiteSpace($shutdown.reason) -or
        $shutdown.session_id -ne $welcome.session_id) {
    throw 'RPC manifest or golden handshake/lifecycle fixtures drifted.'
}

Write-Host 'RPC session contract verified: typed strict envelopes, bounded streaming decode, exact handshake/correlation, cancellation tombstones, heartbeat/events, and shutdown acknowledgement.'
