use crate::ipc::named_pipe_client::{
    CoreRpcClient, CoreRpcClientDiagnostics, CoreRpcClientError, CoreRpcEvent,
};
use crate::session::event_hub::{
    EventFilter, EventHub, EventHubDiagnostics, EventHubError, EventSubscription,
};
use crate::session::snapshot_cache::{
    SnapshotCache, SnapshotCacheError, SnapshotIndex, SnapshotQuery, SnapshotQueryCursor,
    SnapshotQueryPage,
};
use serde::Serialize;
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet};
use std::fmt;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};
use uexplorer_protocol::{
    ResponsePayload, ShutdownPayload, SnapshotCursor, SnapshotPage, WelcomePayload,
    MAX_SNAPSHOT_PAGE_RECORDS, MAX_SNAPSHOT_SOURCE_OBJECTS,
};

pub const MAX_MANAGED_SESSIONS: usize = 16;

const EVENT_FORWARD_WAIT: Duration = Duration::from_millis(50);
const WORKER_CLEANUP_TIMEOUT: Duration = Duration::from_secs(5);
const MAX_SNAPSHOT_RESTARTS: usize = 3;
const MAX_SNAPSHOT_PAGE_REQUESTS: usize = MAX_SNAPSHOT_SOURCE_OBJECTS as usize + 1;
const MAX_PROCESS_PATH_BYTES: usize = 32_768;

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum SessionPhase {
    Ready,
    Closing,
    Closed,
    Failed,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
pub struct TargetProcessIdentity {
    pid: u32,
    start_time_100ns: String,
    process_path: String,
}

impl TargetProcessIdentity {
    pub fn new(
        pid: u32,
        start_time_100ns: u64,
        process_path: impl Into<String>,
    ) -> Result<Self, SessionManagerError> {
        let process_path = process_path.into();
        if pid == 0 || start_time_100ns == 0 {
            return Err(SessionManagerError::InvalidConfiguration(
                "target process identity requires positive PID and start time",
            ));
        }
        if process_path.is_empty()
            || process_path.len() > MAX_PROCESS_PATH_BYTES
            || process_path.chars().any(char::is_control)
        {
            return Err(SessionManagerError::InvalidConfiguration(
                "target process path is empty, oversized, or contains control characters",
            ));
        }
        Ok(Self {
            pid,
            start_time_100ns: start_time_100ns.to_string(),
            process_path,
        })
    }

    pub fn pid(&self) -> u32 {
        self.pid
    }

    pub fn start_time_100ns(&self) -> &str {
        &self.start_time_100ns
    }

    pub fn process_path(&self) -> &str {
        &self.process_path
    }
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
pub struct SessionFailure {
    pub code: String,
    pub message: String,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
pub struct ManagedSessionDiagnostics {
    pub target_pid: u32,
    pub target_start_time_100ns: String,
    pub target_process_path: String,
    pub phase: SessionPhase,
    pub failure: Option<SessionFailure>,
    pub pipe_name: String,
    pub server_pid: u32,
    pub core_version: String,
    pub core_session_id: String,
    pub engine_profile: Option<String>,
    pub capabilities: BTreeMap<String, bool>,
    pub transport_dropped_events: u64,
    pub event_hub: EventHubDiagnostics,
    pub snapshot_generation: Option<u64>,
    pub snapshot_context_generation: Option<u64>,
    pub snapshot_record_count: Option<u32>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
pub struct SessionManagerDiagnostics {
    pub active_pid: Option<u32>,
    pub connecting_pids: Vec<u32>,
    pub sessions: Vec<ManagedSessionDiagnostics>,
}

#[derive(Debug)]
pub enum SessionManagerError {
    InvalidConfiguration(&'static str),
    SessionLimitReached,
    SessionAlreadyExists(u32),
    SessionConnectInProgress(u32),
    SessionNotFound(u32),
    SessionNotReady { pid: u32, phase: SessionPhase },
    CapabilityUnavailable(String),
    Client(CoreRpcClientError),
    EventHub(EventHubError),
    Snapshot(SnapshotCacheError),
    CoreDomain { code: String, message: String },
    InvalidSnapshotResponse(String),
    SnapshotRestartLimitReached,
    SnapshotPageLimitReached,
    DeadlineExpired(&'static str),
    WorkerCreate(String),
    WorkerPanicked,
    LockPoisoned(&'static str),
}

impl SessionManagerError {
    pub fn code(&self) -> &str {
        match self {
            Self::InvalidConfiguration(_) => "SESSION_CONFIGURATION_INVALID",
            Self::SessionLimitReached => "SESSION_LIMIT_REACHED",
            Self::SessionAlreadyExists(_) => "SESSION_ALREADY_EXISTS",
            Self::SessionConnectInProgress(_) => "SESSION_CONNECT_IN_PROGRESS",
            Self::SessionNotFound(_) => "SESSION_NOT_FOUND",
            Self::SessionNotReady { .. } => "SESSION_NOT_READY",
            Self::CapabilityUnavailable(_) => "CAPABILITY_UNAVAILABLE",
            Self::Client(error) => error.code(),
            Self::EventHub(error) => error.code(),
            Self::Snapshot(error) => error.code(),
            Self::CoreDomain { code, .. } => code,
            Self::InvalidSnapshotResponse(_) => "SNAPSHOT_RESPONSE_INVALID",
            Self::SnapshotRestartLimitReached => "SNAPSHOT_RESTART_LIMIT_REACHED",
            Self::SnapshotPageLimitReached => "SNAPSHOT_PAGE_LIMIT_REACHED",
            Self::DeadlineExpired(_) => "SESSION_DEADLINE_EXPIRED",
            Self::WorkerCreate(_) => "SESSION_EVENT_WORKER_CREATE_FAILED",
            Self::WorkerPanicked => "SESSION_EVENT_WORKER_PANICKED",
            Self::LockPoisoned(_) => "SESSION_LOCK_POISONED",
        }
    }
}

impl fmt::Display for SessionManagerError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidConfiguration(reason) => {
                write!(formatter, "invalid session configuration: {reason}")
            }
            Self::SessionLimitReached => write!(formatter, "managed session limit reached"),
            Self::SessionAlreadyExists(pid) => {
                write!(formatter, "PID {pid} already has a managed session")
            }
            Self::SessionConnectInProgress(pid) => {
                write!(formatter, "PID {pid} already has a connection attempt")
            }
            Self::SessionNotFound(pid) => write!(formatter, "PID {pid} has no managed session"),
            Self::SessionNotReady { pid, phase } => {
                write!(formatter, "PID {pid} session is not ready ({phase:?})")
            }
            Self::CapabilityUnavailable(capability) => {
                write!(formatter, "Core capability {capability} is unavailable")
            }
            Self::Client(error) => write!(formatter, "Core RPC client error: {error}"),
            Self::EventHub(error) => write!(formatter, "event hub error: {error}"),
            Self::Snapshot(error) => write!(formatter, "snapshot cache error: {error}"),
            Self::CoreDomain { code, message } => write!(formatter, "{code}: {message}"),
            Self::InvalidSnapshotResponse(message) => {
                write!(formatter, "invalid snapshot response: {message}")
            }
            Self::SnapshotRestartLimitReached => {
                write!(formatter, "snapshot generation changed too many times")
            }
            Self::SnapshotPageLimitReached => write!(formatter, "snapshot page limit reached"),
            Self::DeadlineExpired(operation) => write!(formatter, "{operation} deadline expired"),
            Self::WorkerCreate(message) => {
                write!(formatter, "event worker create failed: {message}")
            }
            Self::WorkerPanicked => write!(formatter, "event worker panicked"),
            Self::LockPoisoned(name) => write!(formatter, "{name} lock is poisoned"),
        }
    }
}

impl std::error::Error for SessionManagerError {}

impl From<CoreRpcClientError> for SessionManagerError {
    fn from(error: CoreRpcClientError) -> Self {
        Self::Client(error)
    }
}

impl From<EventHubError> for SessionManagerError {
    fn from(error: EventHubError) -> Self {
        Self::EventHub(error)
    }
}

impl From<SnapshotCacheError> for SessionManagerError {
    fn from(error: SnapshotCacheError) -> Self {
        Self::Snapshot(error)
    }
}

trait RpcTransport: Send + Sync {
    fn diagnostics(&self) -> CoreRpcClientDiagnostics;
    fn request(
        &self,
        operation: &str,
        timeout_ms: u32,
        data: Value,
    ) -> Result<ResponsePayload, CoreRpcClientError>;
    fn recv_event_timeout(
        &self,
        timeout: Duration,
    ) -> Result<Option<CoreRpcEvent>, CoreRpcClientError>;
    fn shutdown(
        &self,
        reason: &str,
        timeout: Duration,
    ) -> Result<ShutdownPayload, CoreRpcClientError>;
    fn disconnect(&self, timeout: Duration) -> Result<(), CoreRpcClientError>;
}

impl RpcTransport for CoreRpcClient {
    fn diagnostics(&self) -> CoreRpcClientDiagnostics {
        CoreRpcClient::diagnostics(self)
    }

    fn request(
        &self,
        operation: &str,
        timeout_ms: u32,
        data: Value,
    ) -> Result<ResponsePayload, CoreRpcClientError> {
        CoreRpcClient::request(self, operation, timeout_ms, data)
    }

    fn recv_event_timeout(
        &self,
        timeout: Duration,
    ) -> Result<Option<CoreRpcEvent>, CoreRpcClientError> {
        CoreRpcClient::recv_event_timeout(self, timeout)
    }

    fn shutdown(
        &self,
        reason: &str,
        timeout: Duration,
    ) -> Result<ShutdownPayload, CoreRpcClientError> {
        CoreRpcClient::shutdown(self, reason, timeout)
    }

    fn disconnect(&self, timeout: Duration) -> Result<(), CoreRpcClientError> {
        CoreRpcClient::disconnect(self, timeout)
    }
}

struct SessionPhaseState {
    phase: SessionPhase,
    failure: Option<SessionFailure>,
}

pub struct ManagedSession {
    target: TargetProcessIdentity,
    client: Arc<dyn RpcTransport>,
    client_identity: CoreRpcClientDiagnostics,
    event_hub: EventHub,
    snapshot_cache: Arc<SnapshotCache>,
    phase: Arc<Mutex<SessionPhaseState>>,
    event_stop: Arc<AtomicBool>,
    event_worker: Mutex<Option<JoinHandle<()>>>,
    refresh_lock: Mutex<()>,
}

impl ManagedSession {
    fn new(
        target: TargetProcessIdentity,
        client: Arc<dyn RpcTransport>,
    ) -> Result<Arc<Self>, SessionManagerError> {
        let target_pid = target.pid();
        let identity = client.diagnostics();
        if identity.target_pid != target_pid
            || identity.server_pid != target_pid
            || identity.welcome.target_pid != target_pid
        {
            let _ = client.disconnect(WORKER_CLEANUP_TIMEOUT);
            return Err(SessionManagerError::InvalidConfiguration(
                "transport identity does not match the target PID",
            ));
        }
        let event_hub = EventHub::new(identity.welcome.session_id.clone())?;
        let snapshot_cache = Arc::new(SnapshotCache::new(identity.welcome.session_id.clone())?);
        let phase = Arc::new(Mutex::new(SessionPhaseState {
            phase: SessionPhase::Ready,
            failure: None,
        }));
        let event_stop = Arc::new(AtomicBool::new(false));

        let worker_client = Arc::clone(&client);
        let worker_hub = event_hub.clone();
        let worker_phase = Arc::clone(&phase);
        let worker_stop = Arc::clone(&event_stop);
        let event_worker = thread::Builder::new()
            .name(format!("uexplorer-event-forwarder-{target_pid}"))
            .spawn(move || {
                forward_events(
                    target_pid,
                    worker_client,
                    worker_hub,
                    worker_phase,
                    worker_stop,
                )
            })
            .map_err(|error| {
                let _ = client.disconnect(WORKER_CLEANUP_TIMEOUT);
                SessionManagerError::WorkerCreate(error.to_string())
            })?;

        Ok(Arc::new(Self {
            target,
            client,
            client_identity: identity,
            event_hub,
            snapshot_cache,
            phase,
            event_stop,
            event_worker: Mutex::new(Some(event_worker)),
            refresh_lock: Mutex::new(()),
        }))
    }

    pub fn target_pid(&self) -> u32 {
        self.target.pid()
    }

    pub fn target_identity(&self) -> &TargetProcessIdentity {
        &self.target
    }

    pub fn session_id(&self) -> &str {
        &self.client_identity.welcome.session_id
    }

    pub fn welcome(&self) -> &WelcomePayload {
        &self.client_identity.welcome
    }

    pub fn subscribe_events(
        &self,
        filter: EventFilter,
        replay_after_seq: Option<u64>,
        capacity: usize,
    ) -> Result<EventSubscription, SessionManagerError> {
        self.require_ready()?;
        Ok(self
            .event_hub
            .subscribe(filter, replay_after_seq, capacity)?)
    }

    pub fn request(
        &self,
        operation: &str,
        timeout_ms: u32,
        data: Value,
    ) -> Result<ResponsePayload, SessionManagerError> {
        self.require_ready()?;
        match self.client.request(operation, timeout_ms, data) {
            Ok(response) => Ok(response),
            Err(error) => {
                if is_terminal_client_error(&error) {
                    self.mark_failed(error.code(), error.to_string())?;
                }
                Err(error.into())
            }
        }
    }

    pub fn refresh_snapshot(
        &self,
        timeout: Duration,
    ) -> Result<Arc<SnapshotIndex>, SessionManagerError> {
        self.require_ready()?;
        if timeout.is_zero() {
            return Err(SessionManagerError::InvalidConfiguration(
                "snapshot timeout must be positive",
            ));
        }
        if self.welcome().capabilities.get("objects.snapshot") != Some(&true) {
            return Err(SessionManagerError::CapabilityUnavailable(
                "objects.snapshot".to_string(),
            ));
        }
        let _refresh_guard = lock(&self.refresh_lock, "snapshot refresh")?;
        let deadline = Instant::now().checked_add(timeout).ok_or(
            SessionManagerError::InvalidConfiguration("snapshot deadline overflow"),
        )?;

        for restart in 0..=MAX_SNAPSHOT_RESTARTS {
            let mut assembler = self.snapshot_cache.assembler()?;
            let mut cursor: Option<SnapshotCursor> = None;
            let mut page_requests = 0usize;
            loop {
                if page_requests >= MAX_SNAPSHOT_PAGE_REQUESTS {
                    return Err(SessionManagerError::SnapshotPageLimitReached);
                }
                page_requests += 1;
                let timeout_ms =
                    request_timeout_ms(deadline, self.welcome().limits.max_timeout_ms)?;
                let response = self.request(
                    "objects.snapshot.page",
                    timeout_ms,
                    json!({
                        "cursor": cursor,
                        "limit": MAX_SNAPSHOT_PAGE_RECORDS,
                    }),
                )?;
                if !response.ok {
                    let error = response.error.ok_or_else(|| {
                        SessionManagerError::InvalidSnapshotResponse(
                            "failure response omitted its error".to_string(),
                        )
                    })?;
                    if error.code == "SNAPSHOT_GENERATION_MISMATCH" {
                        break;
                    }
                    return Err(SessionManagerError::CoreDomain {
                        code: error.code,
                        message: error.message,
                    });
                }
                let page: SnapshotPage =
                    serde_json::from_value(response.data).map_err(|error| {
                        SessionManagerError::InvalidSnapshotResponse(error.to_string())
                    })?;
                let request_cursor = cursor.clone();
                cursor = match assembler.push_page(request_cursor.as_ref(), page) {
                    Ok(cursor) => cursor,
                    Err(SnapshotCacheError::SnapshotGenerationMismatch { .. }) => {
                        break;
                    }
                    Err(error) => return Err(error.into()),
                };
                if cursor.is_none() {
                    let index = assembler.finish()?;
                    if let Some(current) = self.snapshot_cache.current()? {
                        if index.context_generation() != current.context_generation() {
                            return Err(SnapshotCacheError::ContextGenerationMismatch {
                                expected: current.context_generation(),
                                actual: index.context_generation(),
                            }
                            .into());
                        }
                        if index.generation() == current.generation() {
                            return Ok(current);
                        }
                    }
                    return Ok(self.snapshot_cache.publish(index)?);
                }
            }

            if restart == MAX_SNAPSHOT_RESTARTS {
                return Err(SessionManagerError::SnapshotRestartLimitReached);
            }
        }
        Err(SessionManagerError::SnapshotRestartLimitReached)
    }

    pub fn current_snapshot(&self) -> Result<Option<Arc<SnapshotIndex>>, SessionManagerError> {
        self.require_ready()?;
        Ok(self.snapshot_cache.current()?)
    }

    pub fn query_snapshot(
        &self,
        query: &SnapshotQuery,
        cursor: Option<&SnapshotQueryCursor>,
        limit: usize,
    ) -> Result<SnapshotQueryPage, SessionManagerError> {
        self.require_ready()?;
        let index =
            self.snapshot_cache
                .current()?
                .ok_or(SessionManagerError::InvalidSnapshotResponse(
                    "no complete snapshot generation is published".to_string(),
                ))?;
        Ok(index.query(query, cursor, limit)?)
    }

    pub fn diagnostics(&self) -> Result<ManagedSessionDiagnostics, SessionManagerError> {
        let phase = lock(&self.phase, "session phase")?;
        let transport = self.client.diagnostics();
        let event_hub = self.event_hub.diagnostics()?;
        let snapshot = self.snapshot_cache.current()?;
        Ok(ManagedSessionDiagnostics {
            target_pid: self.target.pid(),
            target_start_time_100ns: self.target.start_time_100ns().to_string(),
            target_process_path: self.target.process_path().to_string(),
            phase: phase.phase,
            failure: phase.failure.clone(),
            pipe_name: transport.pipe_name,
            server_pid: transport.server_pid,
            core_version: transport.welcome.core_version,
            core_session_id: transport.welcome.session_id,
            engine_profile: transport.welcome.engine_profile,
            capabilities: transport.welcome.capabilities,
            transport_dropped_events: transport.transport_dropped_events,
            event_hub,
            snapshot_generation: snapshot.as_ref().map(|index| index.generation()),
            snapshot_context_generation: snapshot.as_ref().map(|index| index.context_generation()),
            snapshot_record_count: snapshot.as_ref().map(|index| index.record_count()),
        })
    }

    fn close(&self, reason: &str, timeout: Duration) -> Result<(), SessionManagerError> {
        self.close_internal(reason, timeout, true)
    }

    fn abort_transport(&self, timeout: Duration) -> Result<(), SessionManagerError> {
        self.close_internal("transport_abort", timeout, false)
    }

    fn close_internal(
        &self,
        reason: &str,
        timeout: Duration,
        graceful: bool,
    ) -> Result<(), SessionManagerError> {
        if timeout.is_zero() {
            return Err(SessionManagerError::InvalidConfiguration(
                "session close timeout must be positive",
            ));
        }
        let should_shutdown = {
            let mut phase = lock(&self.phase, "session phase")?;
            match phase.phase {
                SessionPhase::Ready => {
                    phase.phase = SessionPhase::Closing;
                    graceful
                }
                SessionPhase::Failed => {
                    phase.phase = SessionPhase::Closing;
                    false
                }
                SessionPhase::Closing | SessionPhase::Closed => {
                    return Err(SessionManagerError::SessionNotReady {
                        pid: self.target.pid(),
                        phase: phase.phase,
                    })
                }
            }
        };

        let result = if should_shutdown {
            self.client.shutdown(reason, timeout).map(|_| ())
        } else {
            self.client.disconnect(timeout)
        };
        if result.is_err() {
            let _ = self.client.disconnect(WORKER_CLEANUP_TIMEOUT);
        }
        self.event_stop.store(true, Ordering::Release);
        let worker_result = self.join_event_worker();
        let hub_result = self.event_hub.close();

        let mut phase = lock(&self.phase, "session phase")?;
        match result {
            Ok(()) if worker_result.is_ok() && hub_result.is_ok() => {
                phase.phase = SessionPhase::Closed;
                phase.failure = None;
                Ok(())
            }
            Ok(()) => {
                let error = worker_result
                    .err()
                    .or_else(|| hub_result.err().map(SessionManagerError::from))
                    .expect("one close stage failed");
                phase.phase = SessionPhase::Failed;
                phase.failure = Some(failure_from_manager_error(&error));
                Err(error)
            }
            Err(error) => {
                let error = SessionManagerError::Client(error);
                phase.phase = SessionPhase::Failed;
                phase.failure = Some(failure_from_manager_error(&error));
                Err(error)
            }
        }
    }

    fn require_ready(&self) -> Result<(), SessionManagerError> {
        let phase = lock(&self.phase, "session phase")?;
        if phase.phase != SessionPhase::Ready {
            return Err(SessionManagerError::SessionNotReady {
                pid: self.target.pid(),
                phase: phase.phase,
            });
        }
        Ok(())
    }

    fn mark_failed(
        &self,
        code: impl Into<String>,
        message: impl Into<String>,
    ) -> Result<(), SessionManagerError> {
        let mut phase = lock(&self.phase, "session phase")?;
        if phase.phase == SessionPhase::Ready {
            phase.phase = SessionPhase::Failed;
            phase.failure = Some(SessionFailure {
                code: code.into(),
                message: message.into(),
            });
        }
        Ok(())
    }

    fn join_event_worker(&self) -> Result<(), SessionManagerError> {
        if let Some(worker) = lock(&self.event_worker, "event worker")?.take() {
            worker
                .join()
                .map_err(|_| SessionManagerError::WorkerPanicked)?;
        }
        Ok(())
    }
}

impl Drop for ManagedSession {
    fn drop(&mut self) {
        self.event_stop.store(true, Ordering::Release);
        let _ = self.client.disconnect(WORKER_CLEANUP_TIMEOUT);
        if let Ok(worker_slot) = self.event_worker.get_mut() {
            if let Some(worker) = worker_slot.take() {
                let _ = worker.join();
            }
        }
        let _ = self.event_hub.close();
    }
}

pub struct SessionManager {
    state: Mutex<SessionManagerState>,
}

struct SessionManagerState {
    active_pid: Option<u32>,
    connecting: BTreeSet<u32>,
    sessions: BTreeMap<u32, Arc<ManagedSession>>,
}

impl Default for SessionManager {
    fn default() -> Self {
        Self::new()
    }
}

impl SessionManager {
    pub fn new() -> Self {
        Self {
            state: Mutex::new(SessionManagerState {
                active_pid: None,
                connecting: BTreeSet::new(),
                sessions: BTreeMap::new(),
            }),
        }
    }

    pub fn connect(
        &self,
        target: TargetProcessIdentity,
        host_version: impl Into<String>,
        timeout: Duration,
    ) -> Result<Arc<ManagedSession>, SessionManagerError> {
        let target_pid = target.pid();
        self.reserve_connection(target_pid)?;
        let result = CoreRpcClient::connect(target_pid, host_version, timeout)
            .map(|client| Arc::new(client) as Arc<dyn RpcTransport>)
            .map_err(SessionManagerError::from)
            .and_then(|client| ManagedSession::new(target, client));
        self.finish_connection(target_pid, result)
    }

    pub fn get(&self, target_pid: u32) -> Result<Arc<ManagedSession>, SessionManagerError> {
        lock(&self.state, "session manager")?
            .sessions
            .get(&target_pid)
            .cloned()
            .ok_or(SessionManagerError::SessionNotFound(target_pid))
    }

    pub fn active(&self) -> Result<Option<Arc<ManagedSession>>, SessionManagerError> {
        let state = lock(&self.state, "session manager")?;
        Ok(state
            .active_pid
            .and_then(|pid| state.sessions.get(&pid).cloned()))
    }

    pub fn activate(&self, target_pid: u32) -> Result<(), SessionManagerError> {
        let mut state = lock(&self.state, "session manager")?;
        let session = state
            .sessions
            .get(&target_pid)
            .ok_or(SessionManagerError::SessionNotFound(target_pid))?;
        session.require_ready()?;
        state.active_pid = Some(target_pid);
        Ok(())
    }

    pub fn disconnect(
        &self,
        target_pid: u32,
        reason: &str,
        timeout: Duration,
    ) -> Result<(), SessionManagerError> {
        let session = self.get(target_pid)?;
        let result = session.close(reason, timeout);
        let mut state = lock(&self.state, "session manager")?;
        if state
            .sessions
            .get(&target_pid)
            .is_some_and(|current| Arc::ptr_eq(current, &session))
        {
            state.sessions.remove(&target_pid);
        }
        if state.active_pid == Some(target_pid) {
            state.active_pid = state.sessions.keys().next().copied();
        }
        result
    }

    pub fn abort_connection(
        &self,
        target_pid: u32,
        timeout: Duration,
    ) -> Result<(), SessionManagerError> {
        let session = self.get(target_pid)?;
        let result = session.abort_transport(timeout);
        let mut state = lock(&self.state, "session manager")?;
        if state
            .sessions
            .get(&target_pid)
            .is_some_and(|current| Arc::ptr_eq(current, &session))
        {
            state.sessions.remove(&target_pid);
        }
        if state.active_pid == Some(target_pid) {
            state.active_pid = state.sessions.keys().next().copied();
        }
        result
    }

    pub fn diagnostics(&self) -> Result<SessionManagerDiagnostics, SessionManagerError> {
        let (active_pid, connecting_pids, sessions) = {
            let state = lock(&self.state, "session manager")?;
            (
                state.active_pid,
                state.connecting.iter().copied().collect(),
                state.sessions.values().cloned().collect::<Vec<_>>(),
            )
        };
        let sessions = sessions
            .into_iter()
            .map(|session| session.diagnostics())
            .collect::<Result<Vec<_>, _>>()?;
        Ok(SessionManagerDiagnostics {
            active_pid,
            connecting_pids,
            sessions,
        })
    }

    fn reserve_connection(&self, target_pid: u32) -> Result<(), SessionManagerError> {
        if target_pid == 0 {
            return Err(SessionManagerError::InvalidConfiguration(
                "target PID must be positive",
            ));
        }
        let mut state = lock(&self.state, "session manager")?;
        if state.sessions.contains_key(&target_pid) {
            return Err(SessionManagerError::SessionAlreadyExists(target_pid));
        }
        if state.connecting.contains(&target_pid) {
            return Err(SessionManagerError::SessionConnectInProgress(target_pid));
        }
        if state.sessions.len() + state.connecting.len() >= MAX_MANAGED_SESSIONS {
            return Err(SessionManagerError::SessionLimitReached);
        }
        state.connecting.insert(target_pid);
        Ok(())
    }

    fn finish_connection(
        &self,
        target_pid: u32,
        result: Result<Arc<ManagedSession>, SessionManagerError>,
    ) -> Result<Arc<ManagedSession>, SessionManagerError> {
        let mut state = lock(&self.state, "session manager")?;
        state.connecting.remove(&target_pid);
        let session = result?;
        if state.sessions.contains_key(&target_pid) {
            return Err(SessionManagerError::SessionAlreadyExists(target_pid));
        }
        state.sessions.insert(target_pid, Arc::clone(&session));
        if state.active_pid.is_none() {
            state.active_pid = Some(target_pid);
        }
        Ok(session)
    }

    #[cfg(test)]
    fn attach_transport(
        &self,
        target_pid: u32,
        client: Arc<dyn RpcTransport>,
    ) -> Result<Arc<ManagedSession>, SessionManagerError> {
        let target = TargetProcessIdentity::new(
            target_pid,
            u64::from(target_pid) + 1,
            format!(r"C:\Fixture\Target-{target_pid}.exe"),
        )?;
        self.reserve_connection(target_pid)?;
        self.finish_connection(target_pid, ManagedSession::new(target, client))
    }
}

impl Drop for SessionManager {
    fn drop(&mut self) {
        let sessions = match self.state.get_mut() {
            Ok(state) => std::mem::take(&mut state.sessions),
            Err(poisoned) => std::mem::take(&mut poisoned.into_inner().sessions),
        };
        for (_, session) in sessions {
            let _ = session.close("host_exit", WORKER_CLEANUP_TIMEOUT);
        }
    }
}

fn forward_events(
    target_pid: u32,
    client: Arc<dyn RpcTransport>,
    event_hub: EventHub,
    phase: Arc<Mutex<SessionPhaseState>>,
    stop: Arc<AtomicBool>,
) {
    while !stop.load(Ordering::Acquire) {
        match client.recv_event_timeout(EVENT_FORWARD_WAIT) {
            Ok(Some(event)) => {
                if let Err(error) = event_hub.publish(event.event, event.transport_dropped_before) {
                    fail_event_worker(target_pid, &phase, error.code(), error.to_string());
                    let _ = event_hub.close();
                    let _ = client.disconnect(WORKER_CLEANUP_TIMEOUT);
                    break;
                }
            }
            Ok(None) => {}
            Err(error) => {
                let closing = phase
                    .lock()
                    .map(|state| {
                        matches!(state.phase, SessionPhase::Closing | SessionPhase::Closed)
                    })
                    .unwrap_or(false);
                if !closing && !stop.load(Ordering::Acquire) {
                    fail_event_worker(target_pid, &phase, error.code(), error.to_string());
                    let _ = event_hub.close();
                    let _ = client.disconnect(WORKER_CLEANUP_TIMEOUT);
                }
                break;
            }
        }
    }
}

fn fail_event_worker(
    target_pid: u32,
    phase: &Mutex<SessionPhaseState>,
    code: impl Into<String>,
    message: impl Into<String>,
) {
    match phase.lock() {
        Ok(mut state) if state.phase == SessionPhase::Ready => {
            state.phase = SessionPhase::Failed;
            state.failure = Some(SessionFailure {
                code: code.into(),
                message: message.into(),
            });
        }
        Ok(_) => {}
        Err(_) => log::error!("session {target_pid} phase lock poisoned in event worker"),
    }
}

fn request_timeout_ms(deadline: Instant, negotiated_max: u32) -> Result<u32, SessionManagerError> {
    let remaining = deadline.saturating_duration_since(Instant::now());
    if remaining.is_zero() {
        return Err(SessionManagerError::DeadlineExpired("snapshot refresh"));
    }
    Ok(remaining
        .as_millis()
        .saturating_add(1)
        .min(u128::from(negotiated_max)) as u32)
}

fn is_terminal_client_error(error: &CoreRpcClientError) -> bool {
    match error {
        CoreRpcClientError::Transport { .. }
        | CoreRpcClientError::WorkerStopped
        | CoreRpcClientError::WorkerPanicked
        | CoreRpcClientError::PeerPidMismatch { .. } => true,
        CoreRpcClientError::Session { code, .. } => matches!(
            code.as_str(),
            "RPC_PROTOCOL_ERROR"
                | "RPC_JSON_INVALID"
                | "RPC_ENVELOPE_INVALID"
                | "RPC_PEER_PID_MISMATCH"
                | "RPC_SESSION_MISMATCH"
        ),
        _ => false,
    }
}

fn failure_from_manager_error(error: &SessionManagerError) -> SessionFailure {
    SessionFailure {
        code: error.code().to_string(),
        message: error.to_string(),
    }
}

fn lock<'a, T>(
    mutex: &'a Mutex<T>,
    name: &'static str,
) -> Result<MutexGuard<'a, T>, SessionManagerError> {
    mutex
        .lock()
        .map_err(|_| SessionManagerError::LockPoisoned(name))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::session::event_hub::HostEvent;
    use crate::session::snapshot_cache::{ObjectHandle, SnapshotObjectKind, SnapshotRecord};
    use std::collections::VecDeque;
    use std::sync::atomic::AtomicU64;
    use std::sync::mpsc::{self, Receiver, SyncSender};
    use uexplorer_protocol::{EventPayload, ProtocolLimits, ProtocolVersion, RpcTiming};

    struct FakeTransport {
        diagnostics: CoreRpcClientDiagnostics,
        event_tx: SyncSender<CoreRpcEvent>,
        event_rx: Mutex<Receiver<CoreRpcEvent>>,
        snapshot_pages: Mutex<VecDeque<SnapshotPage>>,
        request_id: AtomicU64,
        shutdown_called: AtomicBool,
        disconnected: AtomicBool,
    }

    impl FakeTransport {
        fn new(pid: u32, session_id: &str, snapshot_pages: Vec<SnapshotPage>) -> Arc<Self> {
            let (event_tx, event_rx) = mpsc::sync_channel(32);
            Arc::new(Self {
                diagnostics: CoreRpcClientDiagnostics {
                    target_pid: pid,
                    pipe_name: format!(r"\\.\pipe\UExplorer\v1\{pid}"),
                    server_pid: pid,
                    welcome: WelcomePayload {
                        core_version: "fake-core".to_string(),
                        protocol: ProtocolVersion { major: 1, minor: 0 },
                        session_id: session_id.to_string(),
                        target_pid: pid,
                        engine_profile: Some("fixture".to_string()),
                        capabilities: BTreeMap::from([
                            ("engine.core".to_string(), true),
                            ("objects.snapshot".to_string(), true),
                            ("status.inspect".to_string(), true),
                            ("transport.named_pipe".to_string(), true),
                        ]),
                        limits: ProtocolLimits {
                            max_payload_bytes: 8 * 1024 * 1024,
                            pending_rpc_per_session: 256,
                            game_thread_tasks: 128,
                            hook_event_ring: 8_192,
                            subscriber_events: 1_024,
                            dump_running: 1,
                            max_timeout_ms: 120_000,
                        },
                    },
                    transport_dropped_events: 0,
                },
                event_tx,
                event_rx: Mutex::new(event_rx),
                snapshot_pages: Mutex::new(snapshot_pages.into()),
                request_id: AtomicU64::new(1),
                shutdown_called: AtomicBool::new(false),
                disconnected: AtomicBool::new(false),
            })
        }

        fn emit(&self, event: EventPayload) {
            self.emit_with_transport_drops(event, 0);
        }

        fn emit_with_transport_drops(&self, event: EventPayload, transport_dropped_before: u64) {
            self.event_tx
                .send(CoreRpcEvent {
                    event,
                    transport_dropped_before,
                })
                .unwrap();
        }
    }

    impl RpcTransport for FakeTransport {
        fn diagnostics(&self) -> CoreRpcClientDiagnostics {
            self.diagnostics.clone()
        }

        fn request(
            &self,
            operation: &str,
            _timeout_ms: u32,
            _data: Value,
        ) -> Result<ResponsePayload, CoreRpcClientError> {
            let request_id = self.request_id.fetch_add(1, Ordering::AcqRel);
            let data = if operation == "objects.snapshot.page" {
                serde_json::to_value(self.snapshot_pages.lock().unwrap().pop_front().ok_or_else(
                    || CoreRpcClientError::Transport {
                        code: "FAKE_PAGE_MISSING".to_string(),
                        message: "no fake snapshot page remains".to_string(),
                    },
                )?)
                .unwrap()
            } else {
                json!({"state": "ready"})
            };
            Ok(ResponsePayload {
                ok: true,
                request_id,
                session_id: self.diagnostics.welcome.session_id.clone(),
                error: None,
                data,
                timing: RpcTiming {
                    queued_us: 0,
                    execute_us: 1,
                },
            })
        }

        fn recv_event_timeout(
            &self,
            timeout: Duration,
        ) -> Result<Option<CoreRpcEvent>, CoreRpcClientError> {
            match self.event_rx.lock().unwrap().recv_timeout(timeout) {
                Ok(event) => Ok(Some(event)),
                Err(std::sync::mpsc::RecvTimeoutError::Timeout) => Ok(None),
                Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => {
                    Err(CoreRpcClientError::WorkerStopped)
                }
            }
        }

        fn shutdown(
            &self,
            reason: &str,
            _timeout: Duration,
        ) -> Result<ShutdownPayload, CoreRpcClientError> {
            self.shutdown_called.store(true, Ordering::Release);
            Ok(ShutdownPayload {
                session_id: self.diagnostics.welcome.session_id.clone(),
                reason: reason.to_string(),
            })
        }

        fn disconnect(&self, _timeout: Duration) -> Result<(), CoreRpcClientError> {
            self.disconnected.store(true, Ordering::Release);
            Ok(())
        }
    }

    fn snapshot_page(pid: u32, session_id: &str) -> SnapshotPage {
        SnapshotPage {
            generation: 9,
            context_generation: 42,
            captured_at_monotonic_us: 1,
            capture_duration_us: 2,
            source_object_count: 1,
            record_count: 1,
            skipped_slots: 0,
            items: vec![SnapshotRecord {
                handle: ObjectHandle {
                    session_id: session_id.to_string(),
                    context_generation: 42,
                    index: 0,
                    serial: 1,
                    address: format!("0x{pid:016X}"),
                    class_fingerprint: "A1B2C3D4E5F60708".to_string(),
                },
                name: "Fixture".to_string(),
                full_path: "Object /Game/Fixture.Fixture".to_string(),
                class_path: "Class /Script/CoreUObject.Object".to_string(),
                package_path: "Package /Game/Fixture".to_string(),
                kind: SnapshotObjectKind::Object,
            }],
            has_more: false,
            next_cursor: None,
        }
    }

    fn event(session_id: &str, seq: u64) -> EventPayload {
        EventPayload {
            seq,
            kind: "runtime.ready".to_string(),
            timestamp_us: seq,
            session_id: session_id.to_string(),
            dropped_before: 0,
            data: json!({}),
        }
    }

    #[test]
    fn manager_is_multi_pid_with_one_explicit_active_session() {
        let manager = SessionManager::new();
        let first = FakeTransport::new(101, "session-101", vec![]);
        let second = FakeTransport::new(202, "session-202", vec![]);
        manager
            .attach_transport(101, Arc::clone(&first) as Arc<dyn RpcTransport>)
            .unwrap();
        manager
            .attach_transport(202, Arc::clone(&second) as Arc<dyn RpcTransport>)
            .unwrap();

        assert_eq!(manager.active().unwrap().unwrap().target_pid(), 101);
        manager.activate(202).unwrap();
        assert_eq!(manager.active().unwrap().unwrap().target_pid(), 202);
        assert!(matches!(
            manager.attach_transport(101, first as Arc<dyn RpcTransport>),
            Err(SessionManagerError::SessionAlreadyExists(101))
        ));

        manager
            .disconnect(202, "test_close", Duration::from_secs(1))
            .unwrap();
        assert!(second.shutdown_called.load(Ordering::Acquire));
        assert_eq!(manager.active().unwrap().unwrap().target_pid(), 101);
        manager
            .disconnect(101, "test_close", Duration::from_secs(1))
            .unwrap();
        assert!(manager.active().unwrap().is_none());
    }

    #[test]
    fn transport_abort_never_sends_shutdown_to_an_untrusted_pid_identity() {
        let manager = SessionManager::new();
        let transport = FakeTransport::new(727, "session-727", vec![]);
        manager
            .attach_transport(727, Arc::clone(&transport) as Arc<dyn RpcTransport>)
            .unwrap();

        manager
            .abort_connection(727, Duration::from_secs(1))
            .unwrap();
        assert!(transport.disconnected.load(Ordering::Acquire));
        assert!(!transport.shutdown_called.load(Ordering::Acquire));
        assert!(matches!(
            manager.get(727),
            Err(SessionManagerError::SessionNotFound(727))
        ));
    }

    #[test]
    fn target_process_identity_keeps_start_time_out_of_json_number_space() {
        let identity =
            TargetProcessIdentity::new(77, u64::MAX, r"C:\Fixture\Target-77.exe".to_string())
                .unwrap();
        let serialized = serde_json::to_value(identity).unwrap();
        assert_eq!(serialized["pid"], 77);
        assert_eq!(serialized["start_time_100ns"], u64::MAX.to_string());
        assert_eq!(serialized["process_path"], r"C:\Fixture\Target-77.exe");
    }

    #[test]
    fn connection_reservation_is_exclusive_and_released_after_failure() {
        let manager = SessionManager::new();
        manager.reserve_connection(707).unwrap();
        assert!(matches!(
            manager.reserve_connection(707),
            Err(SessionManagerError::SessionConnectInProgress(707))
        ));
        assert_eq!(manager.diagnostics().unwrap().connecting_pids, vec![707]);

        assert!(matches!(
            manager.finish_connection(
                707,
                Err(SessionManagerError::InvalidConfiguration("fixture failure"))
            ),
            Err(SessionManagerError::InvalidConfiguration("fixture failure"))
        ));
        assert!(manager.diagnostics().unwrap().connecting_pids.is_empty());
    }

    #[test]
    fn managed_session_rejects_inconsistent_welcome_target_identity() {
        let manager = SessionManager::new();
        let mut transport = FakeTransport::new(717, "session-717", vec![]);
        Arc::get_mut(&mut transport)
            .unwrap()
            .diagnostics
            .welcome
            .target_pid = 718;

        assert!(matches!(
            manager.attach_transport(717, Arc::clone(&transport) as Arc<dyn RpcTransport>),
            Err(SessionManagerError::InvalidConfiguration(
                "transport identity does not match the target PID"
            ))
        ));
        assert!(transport.disconnected.load(Ordering::Acquire));
        assert!(manager.diagnostics().unwrap().connecting_pids.is_empty());
    }

    #[test]
    fn event_forwarder_feeds_filtered_bounded_hub() {
        let manager = SessionManager::new();
        let transport = FakeTransport::new(303, "session-303", vec![]);
        let session = manager
            .attach_transport(303, Arc::clone(&transport) as Arc<dyn RpcTransport>)
            .unwrap();
        let subscription = session
            .subscribe_events(EventFilter::default(), None, 4)
            .unwrap();
        transport.emit(event("session-303", 1));

        let HostEvent { event, .. } = subscription.recv_timeout(Duration::from_secs(1)).unwrap();
        assert_eq!(event.seq, 1);
        assert_eq!(session.diagnostics().unwrap().event_hub.published_events, 1);
        manager
            .disconnect(303, "test_close", Duration::from_secs(1))
            .unwrap();
    }

    #[test]
    fn event_forwarder_preserves_transport_drop_evidence() {
        let manager = SessionManager::new();
        let transport = FakeTransport::new(313, "session-313", vec![]);
        let session = manager
            .attach_transport(313, Arc::clone(&transport) as Arc<dyn RpcTransport>)
            .unwrap();
        let subscription = session
            .subscribe_events(EventFilter::default(), None, 4)
            .unwrap();
        transport.emit(event("session-313", 1));
        transport.emit_with_transport_drops(event("session-313", 3), 1);

        assert_eq!(
            subscription
                .recv_timeout(Duration::from_secs(1))
                .unwrap()
                .host_dropped_before,
            0
        );
        assert_eq!(
            subscription
                .recv_timeout(Duration::from_secs(1))
                .unwrap()
                .host_dropped_before,
            1
        );
        let diagnostics = session.diagnostics().unwrap();
        assert_eq!(diagnostics.phase, SessionPhase::Ready);
        assert_eq!(diagnostics.event_hub.transport_dropped_before, 1);

        manager
            .disconnect(313, "test_close", Duration::from_secs(1))
            .unwrap();
    }

    #[test]
    fn snapshot_refresh_publishes_once_and_queries_host_index() {
        let manager = SessionManager::new();
        let page = snapshot_page(404, "session-404");
        let transport = FakeTransport::new(404, "session-404", vec![page.clone(), page]);
        let session = manager
            .attach_transport(404, Arc::clone(&transport) as Arc<dyn RpcTransport>)
            .unwrap();

        let published = session.refresh_snapshot(Duration::from_secs(1)).unwrap();
        assert_eq!(published.generation(), 9);
        assert_eq!(published.record_count(), 1);
        let unchanged = session.refresh_snapshot(Duration::from_secs(1)).unwrap();
        assert!(Arc::ptr_eq(&published, &unchanged));
        let query = session
            .query_snapshot(&SnapshotQuery::default(), None, 128)
            .unwrap();
        assert_eq!(query.matched_count, 1);
        assert_eq!(query.items[0].name, "Fixture");

        manager
            .disconnect(404, "test_close", Duration::from_secs(1))
            .unwrap();
    }

    #[test]
    fn snapshot_refresh_restarts_after_generation_change() {
        let manager = SessionManager::new();
        let mut first = snapshot_page(414, "session-414");
        first.source_object_count = 2;
        first.record_count = 2;
        first.has_more = true;
        first.next_cursor = Some(SnapshotCursor {
            generation: first.generation,
            after_index: first.items[0].handle.index,
        });

        let mut changed = snapshot_page(414, "session-414");
        changed.generation = 10;
        changed.source_object_count = 2;
        changed.record_count = 2;
        changed.items[0].handle.index = 1;
        changed.items[0].handle.address = "0x000000000000019F".to_string();

        let mut stable = snapshot_page(414, "session-414");
        stable.generation = 11;
        let transport = FakeTransport::new(414, "session-414", vec![first, changed, stable]);
        let session = manager
            .attach_transport(414, Arc::clone(&transport) as Arc<dyn RpcTransport>)
            .unwrap();

        let published = session.refresh_snapshot(Duration::from_secs(1)).unwrap();
        assert_eq!(published.generation(), 11);
        assert_eq!(published.record_count(), 1);

        manager
            .disconnect(414, "test_close", Duration::from_secs(1))
            .unwrap();
    }

    #[test]
    fn invalid_forwarded_event_fails_only_its_session() {
        let manager = SessionManager::new();
        let bad = FakeTransport::new(505, "session-505", vec![]);
        let good = FakeTransport::new(606, "session-606", vec![]);
        let bad_session = manager
            .attach_transport(505, Arc::clone(&bad) as Arc<dyn RpcTransport>)
            .unwrap();
        let bad_subscription = bad_session
            .subscribe_events(EventFilter::default(), None, 4)
            .unwrap();
        let good_session = manager
            .attach_transport(606, Arc::clone(&good) as Arc<dyn RpcTransport>)
            .unwrap();
        bad.emit(event("wrong-session", 1));

        let deadline = Instant::now() + Duration::from_secs(1);
        while bad_session.diagnostics().unwrap().phase == SessionPhase::Ready {
            assert!(Instant::now() < deadline, "bad session was not failed");
            thread::sleep(Duration::from_millis(1));
        }
        assert_eq!(
            bad_session.diagnostics().unwrap().phase,
            SessionPhase::Failed
        );
        assert_eq!(
            good_session.diagnostics().unwrap().phase,
            SessionPhase::Ready
        );
        assert!(bad.disconnected.load(Ordering::Acquire));
        assert!(matches!(
            bad_subscription.recv_timeout(Duration::from_secs(1)),
            Err(EventHubError::SubscriptionDisconnected)
        ));

        manager
            .disconnect(505, "test_cleanup", Duration::from_secs(1))
            .unwrap();
        manager
            .disconnect(606, "test_close", Duration::from_secs(1))
            .unwrap();
    }

    #[test]
    fn only_transport_and_protocol_session_errors_are_terminal() {
        assert!(!is_terminal_client_error(
            &CoreRpcClientError::InvalidConfiguration("bad request".to_string())
        ));
        assert!(!is_terminal_client_error(&CoreRpcClientError::Session {
            code: "RPC_CONFIGURATION_INVALID".to_string(),
            message: "bad request".to_string(),
        }));
        assert!(is_terminal_client_error(&CoreRpcClientError::Session {
            code: "RPC_ENVELOPE_INVALID".to_string(),
            message: "bad envelope".to_string(),
        }));
        assert!(is_terminal_client_error(&CoreRpcClientError::Transport {
            code: "RPC_PIPE_DISCONNECTED".to_string(),
            message: "disconnected".to_string(),
        }));
    }
}
