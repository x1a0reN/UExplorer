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

$eventHub = Read-ProjectFile 'frontend\src-tauri\src\session\event_hub.rs'
$sessionManager = Read-ProjectFile 'frontend\src-tauri\src\session\session_manager.rs'
$sessionModule = Read-ProjectFile 'frontend\src-tauri\src\session\mod.rs'
$schema = Read-ProjectFile 'protocol\v1\schema\payload.schema.json' | ConvertFrom-Json

foreach ($token in @(
        'MAX_EVENT_SUBSCRIBERS: usize = 64', 'MAX_SUBSCRIBER_EVENTS: usize = 1_024',
        'MAX_REPLAY_EVENTS: usize = 1_024', 'MAX_REPLAY_EVENT_BYTES: usize = 64 * 1_024',
        'Arc<EventPayload>', 'pub struct EventFilter',
        'watch_ids', 'hook_names', 'pub struct HostEvent', 'host_dropped_before',
        'ReplayUnavailable', 'ReplayExceedsCapacity', 'SequenceRegression',
        'CoreDropRegression', 'try_send', 'subscriber_delivery_drops',
        'a sequence gap is not accounted for by Core or transport drops',
        'selectors_are_host_side_and_do_not_mutate_sequence',
        'slow_subscriber_has_a_monotonic_drop_counter_without_blocking_publish',
        'transport_drops_account_for_sequence_gaps_and_survive_replay',
        'replay_is_exact_or_explicitly_rejected',
        'closing_or_dropping_subscription_releases_waiters_and_capacity')) {
    Assert-Contains $eventHub $token 'Host EventHub sequencing, filtering, replay, or backpressure contract regressed.'
}
Assert-NotContains $eventHub 'sender.send(' 'EventHub introduced a blocking producer path.'
Assert-NotContains $eventHub '.detach(' 'EventHub introduced detached work.'

foreach ($token in @(
        'MAX_MANAGED_SESSIONS: usize = 16', 'connecting: BTreeSet<u32>',
        'sessions: BTreeMap<u32, Arc<ManagedSession>>', 'active_pid: Option<u32>',
        'CoreRpcClient::connect', 'EventHub::new', 'SnapshotCache::new',
        'transport_dropped_before',
        'uexplorer-event-forwarder-', 'JoinHandle<()>', '.join()',
        'objects.snapshot.page', 'SNAPSHOT_GENERATION_MISMATCH',
        'MAX_SNAPSHOT_RESTARTS', 'MAX_SNAPSHOT_PAGE_REQUESTS',
        'self.snapshot_cache.publish(index)', 'CoreRpcClientError::PeerPidMismatch',
        'manager_is_multi_pid_with_one_explicit_active_session',
        'connection_reservation_is_exclusive_and_released_after_failure',
        'managed_session_rejects_inconsistent_welcome_target_identity',
        'event_forwarder_feeds_filtered_bounded_hub',
        'event_forwarder_preserves_transport_drop_evidence',
        'snapshot_refresh_publishes_once_and_queries_host_index',
        'snapshot_refresh_restarts_after_generation_change',
        'invalid_forwarded_event_fails_only_its_session',
        'only_transport_and_protocol_session_errors_are_terminal')) {
    Assert-Contains $sessionManager $token 'Host SessionManager isolation, lifecycle, or snapshot contract regressed.'
}
foreach ($token in @('TcpStream', 'http://', 'runtime.ini', 'uexplorer-dev', '.detach(')) {
    Assert-NotContains $sessionManager $token 'SessionManager gained a legacy transport, token, endpoint recovery, or detached worker.'
}
foreach ($token in @('pub fn event_hub(', 'pub fn snapshot_cache(')) {
    Assert-NotContains $sessionManager $token 'ManagedSession exposed mutable ownership of an internal per-session service.'
}
Assert-Contains $sessionModule '#[cfg(windows)]' 'Windows-only SessionManager platform boundary was removed.'
Assert-Contains $sessionModule 'pub mod session_manager;' 'SessionManager is no longer exported by the Host session module.'

$eventSchema = $schema.'$defs'.event
if ($eventSchema.properties.seq.maximum -ne 9007199254740991 -or
        $eventSchema.properties.timestamp_us.maximum -ne 9007199254740991 -or
        $eventSchema.properties.dropped_before.maximum -ne 9007199254740991 -or
        $eventSchema.properties.kind.pattern -ne '^[a-z][a-z0-9_.-]{0,127}$') {
    throw 'Event schema lost its JavaScript-safe integer or bounded kind contract.'
}

Write-Host 'Host session contract verified: bounded EventHub fan-out/replay, multi-PID isolation, owned event workers, exact Pipe transport, and atomic snapshot refresh.'
