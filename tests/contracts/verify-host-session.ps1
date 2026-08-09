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
$eventBridge = Read-ProjectFile 'frontend\src-tauri\src\session\event_bridge.rs'
$sessionManager = Read-ProjectFile 'frontend\src-tauri\src\session\session_manager.rs'
$sessionModule = Read-ProjectFile 'frontend\src-tauri\src\session\mod.rs'
$tauriHost = Read-ProjectFile 'frontend\src-tauri\src\lib.rs'
$frontendApi = (Read-ProjectFile 'frontend\src\api\index.ts') +
    (Read-ProjectFile 'frontend\src\api\client.ts')
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
        'MAX_EVENT_BRIDGES: usize = 64', 'pub trait EventSink',
        'BTreeMap<u64, EventBridgeWorker>', 'JoinHandle<()>', 'BRIDGE_RECEIVE_WAIT',
        'TAURI_EVENT_DELIVERY_FAILED', 'pub fn unsubscribe(', 'pub fn stop_session(',
        'reap_finished', 'stop_and_join_all',
        'bridge_is_owned_delivers_and_unsubscribes_exactly',
        'failed_sink_is_diagnostic_and_reaped_before_new_admission',
        'stopping_one_pid_does_not_stop_another_pid_bridge',
        'final_javascript_safe_bridge_id_is_usable_before_exhaustion',
        'panicked_bridge_does_not_detach_sibling_workers_during_session_stop')) {
    Assert-Contains $eventBridge $token 'Host-to-Tauri event bridge ownership, limits, or diagnostics regressed.'
}
Assert-NotContains $eventBridge '.detach(' 'Tauri event bridge introduced detached work.'
Assert-NotContains $eventBridge 'sender.send(' 'Tauri event bridge bypassed the bounded EventHub subscription.'

foreach ($token in @(
        'MAX_MANAGED_SESSIONS: usize = 16', 'connecting: BTreeSet<u32>',
        'pub struct TargetProcessIdentity', 'target_start_time_100ns',
        'sessions: BTreeMap<u32, Arc<ManagedSession>>', 'active_pid: Option<u32>',
        'CoreRpcClient::connect', 'EventHub::new', 'SnapshotCache::new',
        'transport_dropped_before',
        'uexplorer-event-forwarder-', 'JoinHandle<()>', '.join()',
        'objects.snapshot.page', 'SNAPSHOT_GENERATION_MISMATCH',
        'MAX_SNAPSHOT_RESTARTS', 'MAX_SNAPSHOT_PAGE_REQUESTS',
        'self.snapshot_cache.publish(index)', 'CoreRpcClientError::PeerPidMismatch',
        'manager_is_multi_pid_with_one_explicit_active_session',
        'transport_abort_never_sends_shutdown_to_an_untrusted_pid_identity',
        'target_process_identity_keeps_start_time_out_of_json_number_space',
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
Assert-Contains $sessionModule 'pub mod event_bridge;' 'Tauri event bridge is no longer exported by the Host session module.'

foreach ($token in @(
        'TauriChannelEventSink(Channel<HostEvent>)', 'async fn inject_and_connect',
        'fn subscribe_session_events(', 'async fn unsubscribe_session_events(',
        'fn event_bridge_diagnostics(', 'let bridge_result = bridges',
        'let session_result = manager', 'SESSION_DISCONNECT_MULTIPLE_FAILURES',
        'coordinator.inner()', '.reserve(pid)',
        'let sessions = Arc::new(SessionManager::new())', '.manage(sessions)',
        '.manage(domain_service)',
        '.manage(Arc::new(EventBridgeManager::new()))')) {
    Assert-Contains $tauriHost $token 'Tauri did not retain the managed session/event bridge boundary.'
}
foreach ($token in @('std::env::current_dir()', 'is_dev_server_running', 'tauri://localhost/index.html')) {
    Assert-NotContains $tauriHost $token 'Tauri Host reintroduced an implicit filesystem or UI fallback.'
}
foreach ($token in @('runtime.ini', 'connection.ini', 'save_connection_settings')) {
    Assert-NotContains $tauriHost $token 'Tauri Host reintroduced legacy endpoint configuration state.'
}
foreach ($token in @(
        'new Channel<HostSessionEvent>()', "'subscribe_session_events'",
        "'unsubscribe_session_events'", "'event_bridge_diagnostics'",
        'host_dropped_before', 'replayAfterSeq', 'bridgeId: diagnostics.bridge_id')) {
    Assert-Contains $frontendApi $token 'React API did not retain the typed Tauri event channel contract.'
}

$eventSchema = $schema.'$defs'.event
if ($eventSchema.properties.seq.maximum -ne 9007199254740991 -or
        $eventSchema.properties.timestamp_us.maximum -ne 9007199254740991 -or
        $eventSchema.properties.dropped_before.maximum -ne 9007199254740991 -or
        $eventSchema.properties.kind.pattern -ne '^[a-z][a-z0-9_.-]{0,127}$') {
    throw 'Event schema lost its JavaScript-safe integer or bounded kind contract.'
}

Write-Host 'Host session contract verified: bounded EventHub fan-out/replay, managed Tauri channels, multi-PID isolation, owned event workers, exact Pipe transport, and atomic snapshot refresh.'
