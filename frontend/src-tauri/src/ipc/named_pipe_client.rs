use crate::ipc::rpc_session::{CoreRpcSession, RpcInbound, RpcSessionError, RpcSessionState};
use serde_json::Value;
use std::collections::BTreeMap;
use std::fmt;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, SyncSender, TrySendError};
use std::sync::{Arc, Mutex, MutexGuard};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};
use uexplorer_protocol::{
    EventPayload, HeartbeatPayload, ResponsePayload, ShutdownPayload, WelcomePayload,
};
use windows::core::{Error as WindowsError, HRESULT, PCWSTR};
use windows::Win32::Foundation::{
    CloseHandle, ERROR_BROKEN_PIPE, ERROR_FILE_NOT_FOUND, ERROR_IO_PENDING, ERROR_NOT_FOUND,
    ERROR_NO_DATA, ERROR_OPERATION_ABORTED, ERROR_PIPE_BUSY, ERROR_PIPE_NOT_CONNECTED,
    ERROR_SEM_TIMEOUT, HANDLE, WAIT_FAILED, WAIT_OBJECT_0, WAIT_TIMEOUT,
};
use windows::Win32::Storage::FileSystem::{
    CreateFileW, ReadFile, WriteFile, FILE_FLAGS_AND_ATTRIBUTES, FILE_FLAG_OVERLAPPED,
    FILE_SHARE_MODE, OPEN_EXISTING, SECURITY_IDENTIFICATION, SECURITY_SQOS_PRESENT,
};
use windows::Win32::System::Pipes::{
    GetNamedPipeServerProcessId, SetNamedPipeHandleState, WaitNamedPipeW, NAMED_PIPE_MODE,
    PIPE_READMODE_BYTE,
};
use windows::Win32::System::Threading::{
    CreateEventW, ResetEvent, SetEvent, WaitForMultipleObjects, WaitForSingleObject,
};
use windows::Win32::System::IO::{CancelIoEx, GetOverlappedResult, OVERLAPPED};

const READ_CHUNK_BYTES: usize = 64 * 1024;
const COMMAND_CAPACITY: usize = 256;
const EVENT_CAPACITY: usize = 1_024;
const COMMAND_ACCEPT_TIMEOUT: Duration = Duration::from_secs(5);
const REQUEST_COMPLETION_GRACE: Duration = Duration::from_secs(1);
const WORKER_POLL_MS: u32 = 10;
const CONTROL_WRITE_TIMEOUT: Duration = Duration::from_secs(5);

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum CoreRpcClientError {
    InvalidConfiguration(String),
    Session { code: String, message: String },
    Transport { code: String, message: String },
    PeerPidMismatch { expected: u32, actual: u32 },
    ConnectTimeout { target_pid: u32 },
    AdmissionDeadlineExpired { operation: &'static str },
    DeadlineExpired { request_id: u64 },
    RequestCancelled { request_id: u64 },
    CommandQueueFull,
    WorkerStopped,
    WaitTimeout { operation: &'static str },
    WorkerPanicked,
}

impl CoreRpcClientError {
    pub fn code(&self) -> &str {
        match self {
            Self::InvalidConfiguration(_) => "RPC_CLIENT_CONFIGURATION_INVALID",
            Self::Session { code, .. } => code,
            Self::Transport { code, .. } => code,
            Self::PeerPidMismatch { .. } => "RPC_PEER_PID_MISMATCH",
            Self::ConnectTimeout { .. } => "RPC_CONNECT_TIMEOUT",
            Self::AdmissionDeadlineExpired { .. } | Self::DeadlineExpired { .. } => {
                "RPC_DEADLINE_EXPIRED"
            }
            Self::RequestCancelled { .. } => "RPC_REQUEST_CANCELLED",
            Self::CommandQueueFull => "RPC_COMMAND_QUEUE_FULL",
            Self::WorkerStopped => "RPC_WORKER_STOPPED",
            Self::WaitTimeout { .. } => "RPC_WAIT_TIMEOUT",
            Self::WorkerPanicked => "RPC_WORKER_PANICKED",
        }
    }

    fn transport(code: &'static str, message: impl Into<String>) -> Self {
        Self::Transport {
            code: code.to_string(),
            message: message.into(),
        }
    }

    fn disconnected(message: impl Into<String>) -> Self {
        Self::transport("RPC_PIPE_DISCONNECTED", message)
    }
}

impl fmt::Display for CoreRpcClientError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidConfiguration(message) => {
                write!(formatter, "invalid RPC client config: {message}")
            }
            Self::Session { code, message } => write!(formatter, "{code}: {message}"),
            Self::Transport { code, message } => write!(formatter, "{code}: {message}"),
            Self::PeerPidMismatch { expected, actual } => write!(
                formatter,
                "named-pipe server PID mismatch: expected {expected}, got {actual}"
            ),
            Self::ConnectTimeout { target_pid } => {
                write!(
                    formatter,
                    "timed out connecting to Core in PID {target_pid}"
                )
            }
            Self::AdmissionDeadlineExpired { operation } => {
                write!(formatter, "{operation} expired before it could be sent")
            }
            Self::DeadlineExpired { request_id } => {
                write!(formatter, "RPC request {request_id} exceeded its deadline")
            }
            Self::RequestCancelled { request_id } => {
                write!(formatter, "RPC request {request_id} was cancelled")
            }
            Self::CommandQueueFull => write!(formatter, "RPC command queue is full"),
            Self::WorkerStopped => write!(formatter, "RPC I/O worker has stopped"),
            Self::WaitTimeout { operation } => {
                write!(formatter, "timed out waiting for {operation}")
            }
            Self::WorkerPanicked => write!(formatter, "RPC I/O worker panicked"),
        }
    }
}

impl std::error::Error for CoreRpcClientError {}

impl From<RpcSessionError> for CoreRpcClientError {
    fn from(error: RpcSessionError) -> Self {
        Self::Session {
            code: error.code().to_string(),
            message: error.to_string(),
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CoreRpcClientDiagnostics {
    pub target_pid: u32,
    pub pipe_name: String,
    pub server_pid: u32,
    pub welcome: WelcomePayload,
    pub dropped_host_events: u64,
}

pub struct PendingRpc {
    request_id: u64,
    receiver: Receiver<Result<ResponsePayload, CoreRpcClientError>>,
}

impl PendingRpc {
    pub fn request_id(&self) -> u64 {
        self.request_id
    }

    pub fn wait(self, timeout: Duration) -> Result<ResponsePayload, CoreRpcClientError> {
        match self.receiver.recv_timeout(timeout) {
            Ok(result) => result,
            Err(RecvTimeoutError::Timeout) => Err(CoreRpcClientError::WaitTimeout {
                operation: "RPC response",
            }),
            Err(RecvTimeoutError::Disconnected) => Err(CoreRpcClientError::WorkerStopped),
        }
    }
}

pub struct CoreRpcClient {
    target_pid: u32,
    pipe_name: String,
    server_pid: u32,
    welcome: WelcomePayload,
    command_tx: SyncSender<WorkerCommand>,
    command_event: Arc<WinEvent>,
    stop_event: Arc<WinEvent>,
    event_rx: Mutex<Receiver<EventPayload>>,
    dropped_events: Arc<AtomicU64>,
    done_rx: Mutex<Option<Receiver<()>>>,
    worker: Mutex<Option<JoinHandle<()>>>,
}

impl CoreRpcClient {
    pub fn connect(
        target_pid: u32,
        host_version: impl Into<String>,
        timeout: Duration,
    ) -> Result<Self, CoreRpcClientError> {
        if target_pid == 0 {
            return Err(CoreRpcClientError::InvalidConfiguration(
                "target PID must be positive".to_string(),
            ));
        }
        if timeout.is_zero() {
            return Err(CoreRpcClientError::InvalidConfiguration(
                "connect timeout must be positive".to_string(),
            ));
        }
        let host_version = host_version.into();
        // Validate before creating any worker-owned Win32 resources.
        CoreRpcSession::new(target_pid, host_version.clone())?;

        let pipe_name = canonical_pipe_name(target_pid);
        let command_event = Arc::new(WinEvent::new(true, false)?);
        let stop_event = Arc::new(WinEvent::new(true, false)?);
        let (command_tx, command_rx) = mpsc::sync_channel(COMMAND_CAPACITY);
        let (event_tx, event_rx) = mpsc::sync_channel(EVENT_CAPACITY);
        let (handshake_tx, handshake_rx) = mpsc::sync_channel(1);
        let (done_tx, done_rx) = mpsc::sync_channel(1);
        let dropped_events = Arc::new(AtomicU64::new(0));

        let worker_command_event = Arc::clone(&command_event);
        let worker_stop_event = Arc::clone(&stop_event);
        let worker_dropped_events = Arc::clone(&dropped_events);
        let worker_pipe_name = pipe_name.clone();
        let worker = thread::Builder::new()
            .name(format!("uexplorer-core-rpc-{target_pid}"))
            .spawn(move || {
                let result = run_worker(WorkerConfig {
                    target_pid,
                    host_version,
                    pipe_name: worker_pipe_name,
                    connect_timeout: timeout,
                    command_rx,
                    command_event: worker_command_event,
                    stop_event: worker_stop_event,
                    event_tx,
                    dropped_events: worker_dropped_events,
                    handshake_tx,
                });
                if let Err(error) = result {
                    log::error!("Core RPC worker stopped: {}", error);
                }
                let _ = done_tx.send(());
            })
            .map_err(|error| {
                CoreRpcClientError::transport("RPC_WORKER_CREATE_FAILED", error.to_string())
            })?;

        let handshake = match handshake_rx.recv_timeout(timeout + REQUEST_COMPLETION_GRACE) {
            Ok(Ok(handshake)) => handshake,
            Ok(Err(error)) => {
                let _ = stop_event.set();
                join_worker(worker)?;
                return Err(error);
            }
            Err(_) => {
                let _ = stop_event.set();
                join_worker(worker)?;
                return Err(CoreRpcClientError::ConnectTimeout { target_pid });
            }
        };

        Ok(Self {
            target_pid,
            pipe_name,
            server_pid: handshake.server_pid,
            welcome: handshake.welcome,
            command_tx,
            command_event,
            stop_event,
            event_rx: Mutex::new(event_rx),
            dropped_events,
            done_rx: Mutex::new(Some(done_rx)),
            worker: Mutex::new(Some(worker)),
        })
    }

    pub fn target_pid(&self) -> u32 {
        self.target_pid
    }

    pub fn welcome(&self) -> &WelcomePayload {
        &self.welcome
    }

    pub fn diagnostics(&self) -> CoreRpcClientDiagnostics {
        CoreRpcClientDiagnostics {
            target_pid: self.target_pid,
            pipe_name: self.pipe_name.clone(),
            server_pid: self.server_pid,
            welcome: self.welcome.clone(),
            dropped_host_events: self.dropped_events.load(Ordering::Acquire),
        }
    }

    pub fn begin_request(
        &self,
        operation: impl Into<String>,
        timeout_ms: u32,
        data: Value,
    ) -> Result<PendingRpc, CoreRpcClientError> {
        self.validate_timeout(timeout_ms, "request")?;
        let deadline = checked_deadline(timeout_ms, "request")?;
        let (started_tx, started_rx) = mpsc::sync_channel(1);
        let (response_tx, response_rx) = mpsc::sync_channel(1);
        self.send_command(WorkerCommand::Request {
            operation: operation.into(),
            deadline,
            data,
            started: started_tx,
            response: response_tx,
        })?;
        let request_id = match receive_command_result(started_rx, "request acceptance") {
            Ok(request_id) => request_id,
            Err(error @ CoreRpcClientError::WaitTimeout { .. }) => {
                let _ = self.stop_event.set();
                return Err(error);
            }
            Err(error) => return Err(error),
        };
        Ok(PendingRpc {
            request_id,
            receiver: response_rx,
        })
    }

    pub fn request(
        &self,
        operation: impl Into<String>,
        timeout_ms: u32,
        data: Value,
    ) -> Result<ResponsePayload, CoreRpcClientError> {
        let pending = self.begin_request(operation, timeout_ms, data)?;
        let request_id = pending.request_id();
        let wait = Duration::from_millis(u64::from(timeout_ms)) + REQUEST_COMPLETION_GRACE;
        match pending.wait(wait) {
            Err(CoreRpcClientError::WaitTimeout { .. }) => {
                let _ = self.cancel(request_id, "caller_wait_timeout");
                Err(CoreRpcClientError::WaitTimeout {
                    operation: "RPC response",
                })
            }
            result => result,
        }
    }

    pub fn cancel(
        &self,
        request_id: u64,
        reason: impl Into<String>,
    ) -> Result<(), CoreRpcClientError> {
        let (reply_tx, reply_rx) = mpsc::sync_channel(1);
        self.send_command(WorkerCommand::Cancel {
            request_id,
            reason: reason.into(),
            reply: reply_tx,
        })?;
        match receive_command_result(reply_rx, "request cancellation") {
            Err(error @ CoreRpcClientError::WaitTimeout { .. }) => {
                let _ = self.stop_event.set();
                Err(error)
            }
            result => result,
        }
    }

    pub fn ping(
        &self,
        nonce: u64,
        timeout_ms: u32,
    ) -> Result<HeartbeatPayload, CoreRpcClientError> {
        self.validate_timeout(timeout_ms, "ping")?;
        let deadline = checked_deadline(timeout_ms, "ping")?;
        let (reply_tx, reply_rx) = mpsc::sync_channel(1);
        self.send_command(WorkerCommand::Ping {
            nonce,
            deadline,
            reply: reply_tx,
        })?;
        match reply_rx
            .recv_timeout(Duration::from_millis(u64::from(timeout_ms)) + REQUEST_COMPLETION_GRACE)
        {
            Ok(result) => result,
            Err(RecvTimeoutError::Timeout) => {
                let _ = self.stop_event.set();
                Err(CoreRpcClientError::WaitTimeout { operation: "Pong" })
            }
            Err(RecvTimeoutError::Disconnected) => Err(CoreRpcClientError::WorkerStopped),
        }
    }

    pub fn try_recv_event(&self) -> Result<Option<EventPayload>, CoreRpcClientError> {
        match lock(&self.event_rx).try_recv() {
            Ok(event) => Ok(Some(event)),
            Err(mpsc::TryRecvError::Empty) => Ok(None),
            Err(mpsc::TryRecvError::Disconnected) => Err(CoreRpcClientError::WorkerStopped),
        }
    }

    pub fn shutdown(
        &self,
        reason: impl Into<String>,
        timeout: Duration,
    ) -> Result<ShutdownPayload, CoreRpcClientError> {
        if timeout.is_zero() {
            return Err(CoreRpcClientError::InvalidConfiguration(
                "shutdown timeout must be positive".to_string(),
            ));
        }
        let started = Instant::now();
        let (reply_tx, reply_rx) = mpsc::sync_channel(1);
        self.send_command(WorkerCommand::Shutdown {
            reason: reason.into(),
            reply: reply_tx,
        })?;
        let acknowledgement = match reply_rx.recv_timeout(timeout) {
            Ok(result) => result?,
            Err(RecvTimeoutError::Timeout) => {
                let _ = self.stop_event.set();
                return Err(CoreRpcClientError::WaitTimeout {
                    operation: "Shutdown acknowledgement",
                });
            }
            Err(RecvTimeoutError::Disconnected) => return Err(CoreRpcClientError::WorkerStopped),
        };
        let remaining = timeout.saturating_sub(started.elapsed());
        self.wait_for_worker(remaining)?;
        Ok(acknowledgement)
    }

    pub fn disconnect(&self, timeout: Duration) -> Result<(), CoreRpcClientError> {
        self.stop_event.set()?;
        self.wait_for_worker(timeout)
    }

    fn send_command(&self, command: WorkerCommand) -> Result<(), CoreRpcClientError> {
        match self.command_tx.try_send(command) {
            Ok(()) => {
                // The worker also polls every 10 ms, so a failed wake cannot strand
                // an already accepted command or make its side effect ambiguous.
                if let Err(error) = self.command_event.set() {
                    log::error!("RPC command wake failed: {}", error);
                }
                Ok(())
            }
            Err(TrySendError::Full(_)) => Err(CoreRpcClientError::CommandQueueFull),
            Err(TrySendError::Disconnected(_)) => Err(CoreRpcClientError::WorkerStopped),
        }
    }

    fn validate_timeout(
        &self,
        timeout_ms: u32,
        operation: &'static str,
    ) -> Result<(), CoreRpcClientError> {
        if timeout_ms == 0 || timeout_ms > self.welcome.limits.max_timeout_ms {
            return Err(CoreRpcClientError::InvalidConfiguration(format!(
                "{operation} timeout must be within 1..={} ms",
                self.welcome.limits.max_timeout_ms
            )));
        }
        Ok(())
    }

    fn wait_for_worker(&self, timeout: Duration) -> Result<(), CoreRpcClientError> {
        let mut done_guard = lock(&self.done_rx);
        if let Some(done_rx) = done_guard.as_ref() {
            match done_rx.recv_timeout(timeout) {
                Ok(()) | Err(RecvTimeoutError::Disconnected) => {
                    *done_guard = None;
                }
                Err(RecvTimeoutError::Timeout) => {
                    return Err(CoreRpcClientError::WaitTimeout {
                        operation: "RPC worker shutdown",
                    })
                }
            }
        }
        drop(done_guard);

        if let Some(worker) = lock(&self.worker).take() {
            join_worker(worker)?;
        }
        Ok(())
    }
}

impl Drop for CoreRpcClient {
    fn drop(&mut self) {
        let _ = self.stop_event.set();
        if let Some(worker) = lock(&self.worker).take() {
            let _ = worker.join();
        }
    }
}

enum WorkerCommand {
    Request {
        operation: String,
        deadline: Instant,
        data: Value,
        started: SyncSender<Result<u64, CoreRpcClientError>>,
        response: SyncSender<Result<ResponsePayload, CoreRpcClientError>>,
    },
    Cancel {
        request_id: u64,
        reason: String,
        reply: SyncSender<Result<(), CoreRpcClientError>>,
    },
    Ping {
        nonce: u64,
        deadline: Instant,
        reply: SyncSender<Result<HeartbeatPayload, CoreRpcClientError>>,
    },
    Shutdown {
        reason: String,
        reply: SyncSender<Result<ShutdownPayload, CoreRpcClientError>>,
    },
}

struct WorkerConfig {
    target_pid: u32,
    host_version: String,
    pipe_name: String,
    connect_timeout: Duration,
    command_rx: Receiver<WorkerCommand>,
    command_event: Arc<WinEvent>,
    stop_event: Arc<WinEvent>,
    event_tx: SyncSender<EventPayload>,
    dropped_events: Arc<AtomicU64>,
    handshake_tx: SyncSender<Result<HandshakeInfo, CoreRpcClientError>>,
}

#[derive(Clone)]
struct HandshakeInfo {
    server_pid: u32,
    welcome: WelcomePayload,
}

struct HandshakeCompletion {
    welcome: WelcomePayload,
    trailing: Vec<RpcInbound>,
}

struct WorkerState {
    pipe: OwnedHandle,
    session: CoreRpcSession,
    command_rx: Receiver<WorkerCommand>,
    command_event: Arc<WinEvent>,
    stop_event: Arc<WinEvent>,
    event_tx: SyncSender<EventPayload>,
    dropped_events: Arc<AtomicU64>,
    requests: BTreeMap<u64, SyncSender<Result<ResponsePayload, CoreRpcClientError>>>,
    pings: BTreeMap<u64, SyncSender<Result<HeartbeatPayload, CoreRpcClientError>>>,
    shutdown: Option<SyncSender<Result<ShutdownPayload, CoreRpcClientError>>>,
    epoch: Instant,
}

fn run_worker(config: WorkerConfig) -> Result<(), CoreRpcClientError> {
    let deadline = Instant::now()
        .checked_add(config.connect_timeout)
        .ok_or_else(|| {
            CoreRpcClientError::InvalidConfiguration("connect deadline overflow".to_string())
        })?;
    let pipe = match open_pipe(
        &config.pipe_name,
        config.target_pid,
        deadline,
        &config.stop_event,
    ) {
        Ok(pipe) => pipe,
        Err(error) => {
            let _ = config.handshake_tx.send(Err(error.clone()));
            return Err(error);
        }
    };
    let server_pid = match server_process_id(pipe.raw()) {
        Ok(server_pid) if server_pid == config.target_pid => server_pid,
        Ok(server_pid) => {
            let error = CoreRpcClientError::PeerPidMismatch {
                expected: config.target_pid,
                actual: server_pid,
            };
            let _ = config.handshake_tx.send(Err(error.clone()));
            return Err(error);
        }
        Err(error) => {
            let _ = config.handshake_tx.send(Err(error.clone()));
            return Err(error);
        }
    };

    let mut session = CoreRpcSession::new(config.target_pid, config.host_version)?;
    let hello = session.start_handshake()?;
    if let Err(error) = write_frame(pipe.raw(), &hello.bytes, &config.stop_event, deadline) {
        let _ = config.handshake_tx.send(Err(error.clone()));
        return Err(error);
    }
    let handshake = match complete_handshake(
        pipe.raw(),
        config.target_pid,
        &mut session,
        &config.stop_event,
        deadline,
    ) {
        Ok(welcome) => welcome,
        Err(error) => {
            let _ = config.handshake_tx.send(Err(error.clone()));
            return Err(error);
        }
    };
    config
        .handshake_tx
        .send(Ok(HandshakeInfo {
            server_pid,
            welcome: handshake.welcome,
        }))
        .map_err(|_| CoreRpcClientError::WorkerStopped)?;

    let mut worker = WorkerState {
        pipe,
        session,
        command_rx: config.command_rx,
        command_event: config.command_event,
        stop_event: config.stop_event,
        event_tx: config.event_tx,
        dropped_events: config.dropped_events,
        requests: BTreeMap::new(),
        pings: BTreeMap::new(),
        shutdown: None,
        epoch: Instant::now(),
    };
    let result = match worker.process_inbound(handshake.trailing) {
        Ok(true) => Ok(()),
        Ok(false) => worker.run(),
        Err(error) => Err(error),
    };
    let terminal_error = result
        .as_ref()
        .err()
        .cloned()
        .unwrap_or_else(|| CoreRpcClientError::disconnected("RPC worker stopped"));
    worker.fail_pending(terminal_error);
    worker.session.disconnect();
    result
}

impl WorkerState {
    fn run(&mut self) -> Result<(), CoreRpcClientError> {
        let mut read = PendingRead::new()?;
        let result = self.run_loop(&mut read);
        let settle = read.cancel_and_settle(self.pipe.raw());
        match (result, settle) {
            (Ok(()), Ok(())) => Ok(()),
            (Err(error), Ok(())) => Err(error),
            (Ok(()), Err(error)) => Err(error),
            (Err(primary), Err(settle)) => Err(CoreRpcClientError::transport(
                "RPC_READ_CLEANUP_FAILED",
                format!("{primary}; overlapped read cleanup failed: {settle}"),
            )),
        }
    }

    fn run_loop(&mut self, read: &mut PendingRead) -> Result<(), CoreRpcClientError> {
        if let Some(bytes_read) = read.issue(self.pipe.raw())? {
            if bytes_read == 0 {
                return Err(CoreRpcClientError::disconnected(
                    "Core closed the pipe before the first RPC command",
                ));
            }
            if self.process_read(&read.buffer[..bytes_read])? {
                return Ok(());
            }
        }

        loop {
            if !read.is_pending() {
                if let Some(bytes_read) = read.issue(self.pipe.raw())? {
                    if bytes_read == 0 {
                        return Err(CoreRpcClientError::disconnected(
                            "Core closed the named pipe",
                        ));
                    }
                    if self.process_read(&read.buffer[..bytes_read])? {
                        return Ok(());
                    }
                    continue;
                }
            }

            self.expire_requests()?;
            self.command_event.reset()?;
            if self.drain_commands()? {
                return Ok(());
            }

            let handles = [
                read.event.raw(),
                self.command_event.raw(),
                self.stop_event.raw(),
            ];
            let wait = unsafe { WaitForMultipleObjects(&handles, false, WORKER_POLL_MS) };
            if wait == WAIT_OBJECT_0 {
                let bytes_read = read.complete(self.pipe.raw())?;
                if bytes_read == 0 {
                    return Err(CoreRpcClientError::disconnected(
                        "Core closed the named pipe",
                    ));
                }
                if self.process_read(&read.buffer[..bytes_read])? {
                    return Ok(());
                }
            } else if wait.0 == WAIT_OBJECT_0.0 + 1 {
                continue;
            } else if wait.0 == WAIT_OBJECT_0.0 + 2 {
                return Ok(());
            } else if wait == WAIT_TIMEOUT {
                continue;
            } else if wait == WAIT_FAILED {
                return Err(last_transport_error("RPC_WAIT_FAILED"));
            } else {
                return Err(CoreRpcClientError::transport(
                    "RPC_WAIT_UNEXPECTED",
                    format!("WaitForMultipleObjects returned 0x{:08X}", wait.0),
                ));
            }
        }
    }

    fn drain_commands(&mut self) -> Result<bool, CoreRpcClientError> {
        loop {
            match self.command_rx.try_recv() {
                Ok(command) => {
                    if self.handle_command(command)? {
                        return Ok(true);
                    }
                }
                Err(mpsc::TryRecvError::Empty) => return Ok(false),
                Err(mpsc::TryRecvError::Disconnected) => return Ok(true),
            }
        }
    }

    fn handle_command(&mut self, command: WorkerCommand) -> Result<bool, CoreRpcClientError> {
        match command {
            WorkerCommand::Request {
                operation,
                deadline,
                data,
                started,
                response,
            } => {
                if Instant::now() >= deadline {
                    let _ = started.send(Err(CoreRpcClientError::AdmissionDeadlineExpired {
                        operation: "request",
                    }));
                    return Ok(false);
                }
                let remaining_timeout_ms = remaining_millis(deadline);
                let now_us = self.now_us();
                let frame =
                    match self
                        .session
                        .start_request(operation, remaining_timeout_ms, data, now_us)
                    {
                        Ok(frame) => frame,
                        Err(error) => {
                            let _ = started.send(Err(error.into()));
                            return Ok(false);
                        }
                    };
                if let Err(error) =
                    write_frame(self.pipe.raw(), &frame.bytes, &self.stop_event, deadline)
                {
                    let reported = if matches!(error, CoreRpcClientError::WaitTimeout { .. }) {
                        CoreRpcClientError::AdmissionDeadlineExpired {
                            operation: "request",
                        }
                    } else {
                        error.clone()
                    };
                    let _ = started.send(Err(reported));
                    return Err(error);
                }
                self.requests.insert(frame.request_id, response);
                let _ = started.send(Ok(frame.request_id));
            }
            WorkerCommand::Cancel {
                request_id,
                reason,
                reply,
            } => {
                let frame = match self
                    .session
                    .cancel_request(request_id, reason, self.now_us())
                {
                    Ok(frame) => frame,
                    Err(error) => {
                        let _ = reply.send(Err(error.into()));
                        return Ok(false);
                    }
                };
                let deadline = Instant::now() + CONTROL_WRITE_TIMEOUT;
                if let Some(waiter) = self.requests.remove(&request_id) {
                    let _ = waiter.send(Err(CoreRpcClientError::RequestCancelled { request_id }));
                }
                if let Err(error) =
                    write_frame(self.pipe.raw(), &frame.bytes, &self.stop_event, deadline)
                {
                    let _ = reply.send(Err(error.clone()));
                    return Err(error);
                }
                let _ = reply.send(Ok(()));
            }
            WorkerCommand::Ping {
                nonce,
                deadline,
                reply,
            } => {
                if Instant::now() >= deadline {
                    let _ = reply.send(Err(CoreRpcClientError::AdmissionDeadlineExpired {
                        operation: "ping",
                    }));
                    return Ok(false);
                }
                let frame =
                    match self
                        .session
                        .start_ping(nonce, remaining_millis(deadline), self.now_us())
                    {
                        Ok(frame) => frame,
                        Err(error) => {
                            let _ = reply.send(Err(error.into()));
                            return Ok(false);
                        }
                    };
                if let Err(error) =
                    write_frame(self.pipe.raw(), &frame.bytes, &self.stop_event, deadline)
                {
                    let reported = if matches!(error, CoreRpcClientError::WaitTimeout { .. }) {
                        CoreRpcClientError::AdmissionDeadlineExpired { operation: "ping" }
                    } else {
                        error.clone()
                    };
                    let _ = reply.send(Err(reported));
                    return Err(error);
                }
                self.pings.insert(frame.request_id, reply);
            }
            WorkerCommand::Shutdown { reason, reply } => {
                let frame = match self.session.start_shutdown(reason) {
                    Ok(frame) => frame,
                    Err(error) => {
                        let _ = reply.send(Err(error.into()));
                        return Ok(false);
                    }
                };
                let deadline = Instant::now() + CONTROL_WRITE_TIMEOUT;
                if let Err(error) =
                    write_frame(self.pipe.raw(), &frame.bytes, &self.stop_event, deadline)
                {
                    let _ = reply.send(Err(error.clone()));
                    return Err(error);
                }
                self.shutdown = Some(reply);
            }
        }
        Ok(false)
    }

    fn process_read(&mut self, bytes: &[u8]) -> Result<bool, CoreRpcClientError> {
        let inbound = self.session.receive(bytes)?;
        self.process_inbound(inbound)
    }

    fn process_inbound(&mut self, inbound: Vec<RpcInbound>) -> Result<bool, CoreRpcClientError> {
        for message in inbound {
            match message {
                RpcInbound::Response(response) => {
                    if let Some(waiter) = self.requests.remove(&response.request_id) {
                        let _ = waiter.send(Ok(response));
                    }
                }
                RpcInbound::Pong {
                    request_id,
                    payload,
                } => {
                    if let Some(waiter) = self.pings.remove(&request_id) {
                        let _ = waiter.send(Ok(payload));
                    } else {
                        return Err(CoreRpcClientError::transport(
                            "RPC_CORRELATION_LOST",
                            format!("Pong {request_id} had no Host completion waiter"),
                        ));
                    }
                }
                RpcInbound::Event(event) => {
                    if self.event_tx.try_send(event).is_err() {
                        self.dropped_events.fetch_add(1, Ordering::AcqRel);
                    }
                }
                RpcInbound::Reply(reply) => {
                    write_frame(
                        self.pipe.raw(),
                        &reply.bytes,
                        &self.stop_event,
                        Instant::now() + CONTROL_WRITE_TIMEOUT,
                    )?;
                }
                RpcInbound::Shutdown(shutdown) => {
                    if let Some(waiter) = self.shutdown.take() {
                        let _ = waiter.send(Ok(shutdown));
                    }
                    return Ok(true);
                }
                RpcInbound::LateResponse { .. } | RpcInbound::LatePong { .. } => {}
                RpcInbound::Ready(_) => {
                    return Err(CoreRpcClientError::transport(
                        "RPC_DUPLICATE_WELCOME",
                        "received a second Welcome after the session became ready",
                    ))
                }
            }
        }
        Ok(false)
    }

    fn expire_requests(&mut self) -> Result<(), CoreRpcClientError> {
        if !matches!(
            self.session.state(),
            RpcSessionState::Ready | RpcSessionState::Closing
        ) {
            return Ok(());
        }
        let expired = self.session.expire_requests(self.now_us())?;
        for expired_request in expired {
            if let Some(waiter) = self.requests.remove(&expired_request.request_id) {
                let _ = waiter.send(Err(CoreRpcClientError::DeadlineExpired {
                    request_id: expired_request.request_id,
                }));
            }
            if let Some(waiter) = self.pings.remove(&expired_request.request_id) {
                let _ = waiter.send(Err(CoreRpcClientError::DeadlineExpired {
                    request_id: expired_request.request_id,
                }));
            }
            if let Some(cancel) = expired_request.cancel {
                write_frame(
                    self.pipe.raw(),
                    &cancel.bytes,
                    &self.stop_event,
                    Instant::now() + CONTROL_WRITE_TIMEOUT,
                )?;
            }
        }
        Ok(())
    }

    fn fail_pending(&mut self, error: CoreRpcClientError) {
        for (_, waiter) in std::mem::take(&mut self.requests) {
            let _ = waiter.send(Err(error.clone()));
        }
        for (_, waiter) in std::mem::take(&mut self.pings) {
            let _ = waiter.send(Err(error.clone()));
        }
        if let Some(waiter) = self.shutdown.take() {
            let _ = waiter.send(Err(error));
        }
    }

    fn now_us(&self) -> u64 {
        self.epoch
            .elapsed()
            .as_micros()
            .try_into()
            .unwrap_or(u64::MAX)
            .max(1)
    }
}

struct PendingRead {
    event: WinEvent,
    overlapped: Box<OVERLAPPED>,
    buffer: Box<[u8; READ_CHUNK_BYTES]>,
    pending: bool,
}

impl PendingRead {
    fn new() -> Result<Self, CoreRpcClientError> {
        let event = WinEvent::new(true, false)?;
        let overlapped = Box::new(OVERLAPPED {
            hEvent: event.raw(),
            ..Default::default()
        });
        Ok(Self {
            event,
            overlapped,
            buffer: Box::new([0; READ_CHUNK_BYTES]),
            pending: false,
        })
    }

    fn is_pending(&self) -> bool {
        self.pending
    }

    fn issue(&mut self, pipe: HANDLE) -> Result<Option<usize>, CoreRpcClientError> {
        if self.pending {
            return Err(CoreRpcClientError::transport(
                "RPC_READ_STATE_INVALID",
                "attempted to issue a second overlapping read",
            ));
        }
        self.event.reset()?;
        *self.overlapped = OVERLAPPED {
            hEvent: self.event.raw(),
            ..Default::default()
        };
        match unsafe {
            ReadFile(
                pipe,
                Some(self.buffer.as_mut_slice()),
                None,
                Some(self.overlapped.as_mut() as *mut OVERLAPPED),
            )
        } {
            Ok(()) => {
                self.pending = true;
                self.complete(pipe).map(Some)
            }
            Err(error) if windows_error_is(&error, ERROR_IO_PENDING.0) => {
                self.pending = true;
                Ok(None)
            }
            Err(error) if is_disconnect_error(&error) => Err(CoreRpcClientError::disconnected(
                windows_error_detail(&error),
            )),
            Err(error) => Err(CoreRpcClientError::transport(
                "RPC_PIPE_READ_FAILED",
                windows_error_detail(&error),
            )),
        }
    }

    fn complete(&mut self, pipe: HANDLE) -> Result<usize, CoreRpcClientError> {
        let mut transferred = 0u32;
        let result = unsafe {
            GetOverlappedResult(
                pipe,
                self.overlapped.as_ref() as *const OVERLAPPED,
                &mut transferred,
                false,
            )
        };
        self.pending = false;
        match result {
            Ok(()) => Ok(transferred as usize),
            Err(error) if is_disconnect_error(&error) => Err(CoreRpcClientError::disconnected(
                windows_error_detail(&error),
            )),
            Err(error) => Err(CoreRpcClientError::transport(
                "RPC_PIPE_READ_FAILED",
                windows_error_detail(&error),
            )),
        }
    }

    fn cancel_and_settle(&mut self, pipe: HANDLE) -> Result<(), CoreRpcClientError> {
        if !self.pending {
            return Ok(());
        }
        let cancel_error = match unsafe {
            CancelIoEx(pipe, Some(self.overlapped.as_ref() as *const OVERLAPPED))
        } {
            Ok(()) => None,
            Err(error)
                if windows_error_is(&error, ERROR_NOT_FOUND.0) || is_disconnect_error(&error) =>
            {
                None
            }
            Err(error) => Some(CoreRpcClientError::transport(
                "RPC_READ_CANCEL_FAILED",
                windows_error_detail(&error),
            )),
        };
        let mut transferred = 0u32;
        let result = unsafe {
            GetOverlappedResult(
                pipe,
                self.overlapped.as_ref() as *const OVERLAPPED,
                &mut transferred,
                true,
            )
        };
        self.pending = false;
        match result {
            Ok(()) => cancel_error.map_or(Ok(()), Err),
            Err(error)
                if windows_error_is(&error, ERROR_OPERATION_ABORTED.0)
                    || is_disconnect_error(&error) =>
            {
                cancel_error.map_or(Ok(()), Err)
            }
            Err(error) => Err(CoreRpcClientError::transport(
                "RPC_READ_SETTLE_FAILED",
                windows_error_detail(&error),
            )),
        }
    }
}

fn complete_handshake(
    pipe: HANDLE,
    target_pid: u32,
    session: &mut CoreRpcSession,
    stop_event: &WinEvent,
    deadline: Instant,
) -> Result<HandshakeCompletion, CoreRpcClientError> {
    let mut read = PendingRead::new()?;
    loop {
        if Instant::now() >= deadline {
            read.cancel_and_settle(pipe)?;
            return Err(CoreRpcClientError::ConnectTimeout { target_pid });
        }
        if let Some(bytes_read) = read.issue(pipe)? {
            if bytes_read == 0 {
                return Err(CoreRpcClientError::disconnected(
                    "Core closed the pipe during Hello/Welcome",
                ));
            }
            if let Some(welcome) = receive_handshake_chunk(session, &read.buffer[..bytes_read])? {
                return Ok(welcome);
            }
            continue;
        }

        let timeout_ms = remaining_millis(deadline);
        let handles = [read.event.raw(), stop_event.raw()];
        let wait = unsafe { WaitForMultipleObjects(&handles, false, timeout_ms) };
        if wait == WAIT_OBJECT_0 {
            let bytes_read = read.complete(pipe)?;
            if bytes_read == 0 {
                return Err(CoreRpcClientError::disconnected(
                    "Core closed the pipe during Hello/Welcome",
                ));
            }
            if let Some(welcome) = receive_handshake_chunk(session, &read.buffer[..bytes_read])? {
                return Ok(welcome);
            }
        } else if wait.0 == WAIT_OBJECT_0.0 + 1 {
            read.cancel_and_settle(pipe)?;
            return Err(CoreRpcClientError::WorkerStopped);
        } else if wait == WAIT_TIMEOUT {
            read.cancel_and_settle(pipe)?;
            return Err(CoreRpcClientError::ConnectTimeout { target_pid });
        } else if wait == WAIT_FAILED {
            read.cancel_and_settle(pipe)?;
            return Err(last_transport_error("RPC_HANDSHAKE_WAIT_FAILED"));
        } else {
            read.cancel_and_settle(pipe)?;
            return Err(CoreRpcClientError::transport(
                "RPC_HANDSHAKE_WAIT_UNEXPECTED",
                format!("WaitForMultipleObjects returned 0x{:08X}", wait.0),
            ));
        }
    }
}

fn receive_handshake_chunk(
    session: &mut CoreRpcSession,
    bytes: &[u8],
) -> Result<Option<HandshakeCompletion>, CoreRpcClientError> {
    let mut inbound = session.receive(bytes)?;
    if inbound.is_empty() {
        return Ok(None);
    }
    match inbound.remove(0) {
        RpcInbound::Ready(welcome) => Ok(Some(HandshakeCompletion {
            welcome,
            trailing: inbound,
        })),
        _ => Err(CoreRpcClientError::transport(
            "RPC_HANDSHAKE_SEQUENCE_INVALID",
            "the first complete handshake frame was not Welcome",
        )),
    }
}

fn open_pipe(
    pipe_name: &str,
    target_pid: u32,
    deadline: Instant,
    stop_event: &WinEvent,
) -> Result<OwnedHandle, CoreRpcClientError> {
    let wide_name = wide_null(pipe_name);
    loop {
        if Instant::now() >= deadline {
            return Err(CoreRpcClientError::ConnectTimeout { target_pid });
        }
        if unsafe { WaitForSingleObject(stop_event.raw(), 0) } == WAIT_OBJECT_0 {
            return Err(CoreRpcClientError::WorkerStopped);
        }
        let flags = FILE_FLAGS_AND_ATTRIBUTES(
            FILE_FLAG_OVERLAPPED.0 | SECURITY_SQOS_PRESENT.0 | SECURITY_IDENTIFICATION.0,
        );
        match unsafe {
            CreateFileW(
                PCWSTR(wide_name.as_ptr()),
                windows::Win32::Foundation::GENERIC_READ.0
                    | windows::Win32::Foundation::GENERIC_WRITE.0,
                FILE_SHARE_MODE(0),
                None,
                OPEN_EXISTING,
                flags,
                HANDLE::default(),
            )
        } {
            Ok(handle) => {
                let pipe = OwnedHandle::new(handle)?;
                let mode = NAMED_PIPE_MODE(PIPE_READMODE_BYTE.0);
                unsafe { SetNamedPipeHandleState(pipe.raw(), Some(&mode), None, None) }.map_err(
                    |error| {
                        CoreRpcClientError::transport(
                            "RPC_PIPE_MODE_FAILED",
                            windows_error_detail(&error),
                        )
                    },
                )?;
                return Ok(pipe);
            }
            Err(error)
                if windows_error_is(&error, ERROR_FILE_NOT_FOUND.0)
                    || windows_error_is(&error, ERROR_PIPE_BUSY.0) =>
            {
                let remaining = remaining_millis(deadline).min(50);
                if windows_error_is(&error, ERROR_PIPE_BUSY.0) {
                    let _ = unsafe { WaitNamedPipeW(PCWSTR(wide_name.as_ptr()), remaining) };
                }
                let wait = unsafe { WaitForSingleObject(stop_event.raw(), remaining.max(1)) };
                if wait == WAIT_OBJECT_0 {
                    return Err(CoreRpcClientError::WorkerStopped);
                }
                if wait == WAIT_FAILED {
                    return Err(last_transport_error("RPC_CONNECT_WAIT_FAILED"));
                }
            }
            Err(error) if windows_error_is(&error, ERROR_SEM_TIMEOUT.0) => {}
            Err(error) => {
                return Err(CoreRpcClientError::transport(
                    "RPC_PIPE_OPEN_FAILED",
                    windows_error_detail(&error),
                ))
            }
        }
    }
}

fn server_process_id(pipe: HANDLE) -> Result<u32, CoreRpcClientError> {
    let mut server_pid = 0u32;
    unsafe { GetNamedPipeServerProcessId(pipe, &mut server_pid) }.map_err(|error| {
        CoreRpcClientError::transport("RPC_SERVER_PID_QUERY_FAILED", windows_error_detail(&error))
    })?;
    if server_pid == 0 {
        return Err(CoreRpcClientError::transport(
            "RPC_SERVER_PID_INVALID",
            "GetNamedPipeServerProcessId returned zero",
        ));
    }
    Ok(server_pid)
}

fn write_frame(
    pipe: HANDLE,
    bytes: &[u8],
    stop_event: &WinEvent,
    deadline: Instant,
) -> Result<(), CoreRpcClientError> {
    let event = WinEvent::new(true, false)?;
    let mut offset = 0usize;
    while offset < bytes.len() {
        if Instant::now() >= deadline {
            return Err(CoreRpcClientError::WaitTimeout {
                operation: "named-pipe write",
            });
        }
        event.reset()?;
        let mut overlapped = Box::new(OVERLAPPED {
            hEvent: event.raw(),
            ..Default::default()
        });
        let write_end = (offset + READ_CHUNK_BYTES).min(bytes.len());
        let chunk = &bytes[offset..write_end];
        let result = unsafe {
            WriteFile(
                pipe,
                Some(chunk),
                None,
                Some(overlapped.as_mut() as *mut OVERLAPPED),
            )
        };
        let pending = match result {
            Ok(()) => false,
            Err(error) if windows_error_is(&error, ERROR_IO_PENDING.0) => true,
            Err(error) if is_disconnect_error(&error) => {
                return Err(CoreRpcClientError::disconnected(windows_error_detail(
                    &error,
                )))
            }
            Err(error) => {
                return Err(CoreRpcClientError::transport(
                    "RPC_PIPE_WRITE_FAILED",
                    windows_error_detail(&error),
                ))
            }
        };
        if pending {
            let handles = [event.raw(), stop_event.raw()];
            let wait =
                unsafe { WaitForMultipleObjects(&handles, false, remaining_millis(deadline)) };
            if wait.0 == WAIT_OBJECT_0.0 + 1 || wait == WAIT_TIMEOUT {
                cancel_and_settle_write(pipe, overlapped.as_ref())?;
                return Err(if wait == WAIT_TIMEOUT {
                    CoreRpcClientError::WaitTimeout {
                        operation: "named-pipe write",
                    }
                } else {
                    CoreRpcClientError::WorkerStopped
                });
            }
            if wait == WAIT_FAILED {
                cancel_and_settle_write(pipe, overlapped.as_ref())?;
                return Err(last_transport_error("RPC_WRITE_WAIT_FAILED"));
            }
            if wait != WAIT_OBJECT_0 {
                cancel_and_settle_write(pipe, overlapped.as_ref())?;
                return Err(CoreRpcClientError::transport(
                    "RPC_WRITE_WAIT_UNEXPECTED",
                    format!("WaitForMultipleObjects returned 0x{:08X}", wait.0),
                ));
            }
        }

        let mut transferred = 0u32;
        unsafe {
            GetOverlappedResult(
                pipe,
                overlapped.as_ref() as *const OVERLAPPED,
                &mut transferred,
                false,
            )
        }
        .map_err(|error| {
            if is_disconnect_error(&error) {
                CoreRpcClientError::disconnected(windows_error_detail(&error))
            } else {
                CoreRpcClientError::transport("RPC_PIPE_WRITE_FAILED", windows_error_detail(&error))
            }
        })?;
        if transferred == 0 || transferred as usize > chunk.len() {
            return Err(CoreRpcClientError::transport(
                "RPC_PIPE_WRITE_INCOMPLETE",
                format!(
                    "WriteFile completed {transferred} bytes for a {}-byte chunk",
                    chunk.len()
                ),
            ));
        }
        offset += transferred as usize;
    }
    Ok(())
}

fn cancel_and_settle_write(
    pipe: HANDLE,
    overlapped: &OVERLAPPED,
) -> Result<(), CoreRpcClientError> {
    let cancel_error = match unsafe { CancelIoEx(pipe, Some(overlapped as *const OVERLAPPED)) } {
        Ok(()) => None,
        Err(error)
            if windows_error_is(&error, ERROR_NOT_FOUND.0) || is_disconnect_error(&error) =>
        {
            None
        }
        Err(error) => Some(CoreRpcClientError::transport(
            "RPC_WRITE_CANCEL_FAILED",
            windows_error_detail(&error),
        )),
    };
    let mut transferred = 0u32;
    match unsafe { GetOverlappedResult(pipe, overlapped, &mut transferred, true) } {
        Ok(()) => cancel_error.map_or(Ok(()), Err),
        Err(error)
            if windows_error_is(&error, ERROR_OPERATION_ABORTED.0)
                || is_disconnect_error(&error) =>
        {
            cancel_error.map_or(Ok(()), Err)
        }
        Err(error) => Err(CoreRpcClientError::transport(
            "RPC_WRITE_SETTLE_FAILED",
            windows_error_detail(&error),
        )),
    }
}

struct OwnedHandle(HANDLE);

impl OwnedHandle {
    fn new(handle: HANDLE) -> Result<Self, CoreRpcClientError> {
        if handle.is_invalid() {
            Err(last_transport_error("RPC_HANDLE_INVALID"))
        } else {
            Ok(Self(handle))
        }
    }

    fn raw(&self) -> HANDLE {
        self.0
    }
}

impl Drop for OwnedHandle {
    fn drop(&mut self) {
        unsafe {
            let _ = CloseHandle(self.0);
        }
    }
}

struct WinEvent(OwnedHandle);

// Win32 event operations are thread-safe; Arc keeps the handle alive for every user.
unsafe impl Send for WinEvent {}
unsafe impl Sync for WinEvent {}

impl WinEvent {
    fn new(manual_reset: bool, initial_state: bool) -> Result<Self, CoreRpcClientError> {
        let handle = unsafe { CreateEventW(None, manual_reset, initial_state, PCWSTR::null()) }
            .map_err(|error| {
                CoreRpcClientError::transport(
                    "RPC_EVENT_CREATE_FAILED",
                    windows_error_detail(&error),
                )
            })?;
        Ok(Self(OwnedHandle::new(handle)?))
    }

    fn raw(&self) -> HANDLE {
        self.0.raw()
    }

    fn set(&self) -> Result<(), CoreRpcClientError> {
        unsafe { SetEvent(self.raw()) }.map_err(|error| {
            CoreRpcClientError::transport("RPC_EVENT_SET_FAILED", windows_error_detail(&error))
        })
    }

    fn reset(&self) -> Result<(), CoreRpcClientError> {
        unsafe { ResetEvent(self.raw()) }.map_err(|error| {
            CoreRpcClientError::transport("RPC_EVENT_RESET_FAILED", windows_error_detail(&error))
        })
    }
}

fn receive_command_result<T>(
    receiver: Receiver<Result<T, CoreRpcClientError>>,
    operation: &'static str,
) -> Result<T, CoreRpcClientError> {
    match receiver.recv_timeout(COMMAND_ACCEPT_TIMEOUT) {
        Ok(result) => result,
        Err(RecvTimeoutError::Timeout) => Err(CoreRpcClientError::WaitTimeout { operation }),
        Err(RecvTimeoutError::Disconnected) => Err(CoreRpcClientError::WorkerStopped),
    }
}

fn join_worker(worker: JoinHandle<()>) -> Result<(), CoreRpcClientError> {
    worker
        .join()
        .map_err(|_| CoreRpcClientError::WorkerPanicked)
}

fn canonical_pipe_name(target_pid: u32) -> String {
    format!(r"\\.\pipe\UExplorer\v1\{target_pid}")
}

fn wide_null(value: &str) -> Vec<u16> {
    value.encode_utf16().chain(std::iter::once(0)).collect()
}

fn checked_deadline(
    timeout_ms: u32,
    operation: &'static str,
) -> Result<Instant, CoreRpcClientError> {
    Instant::now()
        .checked_add(Duration::from_millis(u64::from(timeout_ms)))
        .ok_or_else(|| {
            CoreRpcClientError::InvalidConfiguration(format!("{operation} deadline overflow"))
        })
}

fn remaining_millis(deadline: Instant) -> u32 {
    let remaining = deadline.saturating_duration_since(Instant::now());
    if remaining.is_zero() {
        return 0;
    }
    remaining
        .as_millis()
        .saturating_add(1)
        .min(u128::from(u32::MAX)) as u32
}

fn windows_error_is(error: &WindowsError, code: u32) -> bool {
    error.code() == HRESULT::from_win32(code)
}

fn is_disconnect_error(error: &WindowsError) -> bool {
    windows_error_is(error, ERROR_BROKEN_PIPE.0)
        || windows_error_is(error, ERROR_NO_DATA.0)
        || windows_error_is(error, ERROR_PIPE_NOT_CONNECTED.0)
}

fn windows_error_detail(error: &WindowsError) -> String {
    format!("HRESULT=0x{:08X}: {}", error.code().0 as u32, error)
}

fn last_transport_error(code: &'static str) -> CoreRpcClientError {
    let error = WindowsError::from_win32();
    CoreRpcClientError::transport(code, windows_error_detail(&error))
}

fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use std::sync::OnceLock;
    use uexplorer_fake_core::FakeCore;
    use uexplorer_protocol::{encode_frame, EventPayload, FrameDecoder, FrameKind};
    use windows::Win32::Foundation::ERROR_PIPE_CONNECTED;
    use windows::Win32::Storage::FileSystem::{
        FlushFileBuffers, FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_ACCESS_DUPLEX,
    };
    use windows::Win32::System::Pipes::{
        ConnectNamedPipe, CreateNamedPipeW, NAMED_PIPE_MODE, PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_TYPE_BYTE, PIPE_WAIT,
    };

    #[derive(Clone, Copy, Eq, PartialEq)]
    enum ServerMode {
        Normal,
        FragmentResponses,
        BurstEvents,
        HoldRequests,
        CloseOnRequest,
    }

    struct TestServer {
        worker: JoinHandle<Result<(), String>>,
    }

    impl TestServer {
        fn join(self) {
            self.worker
                .join()
                .expect("test server thread must not panic")
                .expect("test server must complete cleanly");
        }
    }

    fn test_lock() -> MutexGuard<'static, ()> {
        static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
        lock(LOCK.get_or_init(|| Mutex::new(())))
    }

    fn spawn_server(target_pid: u32, mode: ServerMode) -> TestServer {
        let pipe_name = canonical_pipe_name(target_pid);
        let (ready_tx, ready_rx) = mpsc::sync_channel(1);
        let worker = thread::Builder::new()
            .name(format!("uexplorer-test-core-{target_pid}"))
            .spawn(move || run_test_server(&pipe_name, target_pid, mode, ready_tx))
            .expect("test server thread must start");
        ready_rx
            .recv_timeout(Duration::from_secs(5))
            .expect("test server must report readiness")
            .expect("test server must create its pipe");
        TestServer { worker }
    }

    fn run_test_server(
        pipe_name: &str,
        target_pid: u32,
        mode: ServerMode,
        ready: SyncSender<Result<(), String>>,
    ) -> Result<(), String> {
        let wide_name = wide_null(pipe_name);
        let open_mode =
            FILE_FLAGS_AND_ATTRIBUTES(PIPE_ACCESS_DUPLEX.0 | FILE_FLAG_FIRST_PIPE_INSTANCE.0);
        let pipe_mode = NAMED_PIPE_MODE(
            PIPE_TYPE_BYTE.0 | PIPE_READMODE_BYTE.0 | PIPE_WAIT.0 | PIPE_REJECT_REMOTE_CLIENTS.0,
        );
        let handle = unsafe {
            CreateNamedPipeW(
                PCWSTR(wide_name.as_ptr()),
                open_mode,
                pipe_mode,
                1,
                READ_CHUNK_BYTES as u32,
                READ_CHUNK_BYTES as u32,
                0,
                None,
            )
        };
        if handle.is_invalid() {
            let detail = windows_error_detail(&WindowsError::from_win32());
            let _ = ready.send(Err(detail.clone()));
            return Err(detail);
        }
        let pipe = OwnedHandle::new(handle).map_err(|error| error.to_string())?;
        let _ = ready.send(Ok(()));

        match unsafe { ConnectNamedPipe(pipe.raw(), None) } {
            Ok(()) => {}
            Err(error) if windows_error_is(&error, ERROR_PIPE_CONNECTED.0) => {}
            Err(error) => return Err(windows_error_detail(&error)),
        }

        let mut core = FakeCore::new(target_pid);
        let mut observer = FrameDecoder::default();
        let mut buffer = [0u8; READ_CHUNK_BYTES];
        loop {
            let mut bytes_read = 0u32;
            match unsafe { ReadFile(pipe.raw(), Some(&mut buffer), Some(&mut bytes_read), None) } {
                Ok(()) if bytes_read == 0 => return Ok(()),
                Ok(()) => {}
                Err(error) if is_disconnect_error(&error) => return Ok(()),
                Err(error) => return Err(windows_error_detail(&error)),
            }
            let input = &buffer[..bytes_read as usize];
            let observed = observer.push(input).map_err(|error| error.to_string())?;
            let close_on_request = mode == ServerMode::CloseOnRequest
                && observed
                    .iter()
                    .any(|frame| frame.header.kind == FrameKind::Request);
            if close_on_request {
                return Ok(());
            }
            let saw_shutdown = observed
                .iter()
                .any(|frame| frame.header.kind == FrameKind::Shutdown);
            let hold_request = mode == ServerMode::HoldRequests
                && observed
                    .iter()
                    .any(|frame| frame.header.kind == FrameKind::Request);
            let outputs = if hold_request {
                Vec::new()
            } else {
                core.accept(input).map_err(|error| error.to_string())?
            };
            let mut output: Vec<u8> = outputs.into_iter().flatten().collect();
            if observed
                .iter()
                .any(|frame| frame.header.kind == FrameKind::Hello)
            {
                let event_count = if mode == ServerMode::BurstEvents {
                    EVENT_CAPACITY + 2
                } else if mode == ServerMode::FragmentResponses {
                    1
                } else {
                    0
                };
                for seq in 1..=event_count {
                    let event = EventPayload {
                        seq: seq as u64,
                        kind: "runtime.ready".to_string(),
                        timestamp_us: seq as u64,
                        session_id: format!("fake-session-{target_pid}"),
                        dropped_before: 0,
                        data: json!({ "ready": true }),
                    };
                    output.extend(
                        encode_frame(
                            FrameKind::Event,
                            0,
                            &serde_json::to_vec(&event).map_err(|error| error.to_string())?,
                        )
                        .map_err(|error| error.to_string())?,
                    );
                }
            }
            if mode == ServerMode::FragmentResponses {
                for byte in output {
                    write_test_server_bytes(pipe.raw(), &[byte])?;
                }
            } else {
                write_test_server_bytes(pipe.raw(), &output)?;
            }
            if saw_shutdown {
                unsafe { FlushFileBuffers(pipe.raw()) }
                    .map_err(|error| windows_error_detail(&error))?;
                return Ok(());
            }
        }
    }

    fn write_test_server_bytes(pipe: HANDLE, bytes: &[u8]) -> Result<(), String> {
        let mut offset = 0usize;
        while offset < bytes.len() {
            let mut written = 0u32;
            unsafe { WriteFile(pipe, Some(&bytes[offset..]), Some(&mut written), None) }
                .map_err(|error| windows_error_detail(&error))?;
            if written == 0 {
                return Err("test server WriteFile completed zero bytes".to_string());
            }
            offset += written as usize;
        }
        Ok(())
    }

    fn different_pid(pid: u32) -> u32 {
        if pid < u32::MAX - 1_000_000 {
            pid + 1_000_000
        } else {
            pid - 1
        }
    }

    #[test]
    fn real_named_pipe_lifecycle_verifies_peer_and_joins_worker() {
        let _guard = test_lock();
        let pid = std::process::id();
        let server = spawn_server(pid, ServerMode::FragmentResponses);
        let client = CoreRpcClient::connect(pid, "test-host", Duration::from_secs(5)).unwrap();

        let diagnostics = client.diagnostics();
        assert_eq!(diagnostics.target_pid, pid);
        assert_eq!(diagnostics.server_pid, pid);
        assert_eq!(diagnostics.pipe_name, canonical_pipe_name(pid));
        assert_eq!(diagnostics.welcome.target_pid, pid);

        let event_deadline = Instant::now() + Duration::from_secs(5);
        let event = loop {
            if let Some(event) = client.try_recv_event().unwrap() {
                break event;
            }
            assert!(
                Instant::now() < event_deadline,
                "expected runtime.ready event"
            );
            thread::sleep(Duration::from_millis(1));
        };
        assert_eq!(event.seq, 1);
        assert_eq!(event.kind, "runtime.ready");

        let response = client.request("status.inspect", 5_000, json!({})).unwrap();
        assert!(response.ok);
        assert_eq!(response.data["state"], "ready");

        let client = Arc::new(client);
        let first = {
            let client = Arc::clone(&client);
            thread::spawn(move || client.ping(41, 5_000))
        };
        let second = {
            let client = Arc::clone(&client);
            thread::spawn(move || client.ping(42, 5_000))
        };
        assert_eq!(first.join().unwrap().unwrap().nonce, 41);
        assert_eq!(second.join().unwrap().unwrap().nonce, 42);

        let acknowledgement = client
            .shutdown("host_exit", Duration::from_secs(5))
            .unwrap();
        assert_eq!(acknowledgement.reason, "host_exit");
        server.join();

        let restarted_server = spawn_server(pid, ServerMode::Normal);
        let restarted = CoreRpcClient::connect(pid, "test-host", Duration::from_secs(5)).unwrap();
        assert!(
            restarted
                .request("status.inspect", 5_000, json!({}))
                .unwrap()
                .ok
        );
        restarted
            .shutdown("host_restart_test", Duration::from_secs(5))
            .unwrap();
        restarted_server.join();
    }

    #[test]
    fn rejects_pipe_server_pid_mismatch_before_hello() {
        let _guard = test_lock();
        let actual_pid = std::process::id();
        let target_pid = different_pid(actual_pid);
        let server = spawn_server(target_pid, ServerMode::Normal);

        let error = CoreRpcClient::connect(target_pid, "test-host", Duration::from_secs(5))
            .err()
            .expect("client must reject an unexpected server process");
        assert_eq!(
            error,
            CoreRpcClientError::PeerPidMismatch {
                expected: target_pid,
                actual: actual_pid,
            }
        );
        server.join();
    }

    #[test]
    fn mid_request_disconnect_completes_pending_call_and_worker() {
        let _guard = test_lock();
        let pid = std::process::id();
        let server = spawn_server(pid, ServerMode::CloseOnRequest);
        let client = CoreRpcClient::connect(pid, "test-host", Duration::from_secs(5)).unwrap();

        let error = client
            .request("status.inspect", 5_000, json!({}))
            .expect_err("disconnected request must fail explicitly");
        assert_eq!(error.code(), "RPC_PIPE_DISCONNECTED");
        client.disconnect(Duration::from_secs(5)).unwrap();
        server.join();
    }

    #[test]
    fn event_reader_drops_overflow_without_blocking_rpc() {
        let _guard = test_lock();
        let pid = std::process::id();
        let server = spawn_server(pid, ServerMode::BurstEvents);
        let client = CoreRpcClient::connect(pid, "test-host", Duration::from_secs(5)).unwrap();

        let deadline = Instant::now() + Duration::from_secs(5);
        while client.diagnostics().dropped_host_events < 2 {
            assert!(
                Instant::now() < deadline,
                "bounded event queue did not record overflow"
            );
            thread::sleep(Duration::from_millis(1));
        }
        assert!(
            client
                .request("status.inspect", 5_000, json!({}))
                .unwrap()
                .ok
        );
        client
            .shutdown("event_backpressure_test", Duration::from_secs(5))
            .unwrap();
        server.join();
    }

    #[test]
    fn cancellation_and_deadline_have_one_explicit_completion() {
        let _guard = test_lock();
        let pid = std::process::id();
        let server = spawn_server(pid, ServerMode::HoldRequests);
        let client = CoreRpcClient::connect(pid, "test-host", Duration::from_secs(5)).unwrap();

        let pending = client
            .begin_request("status.inspect", 5_000, json!({}))
            .unwrap();
        let request_id = pending.request_id();
        client.cancel(request_id, "test_cancel").unwrap();
        assert_eq!(
            pending.wait(Duration::from_secs(1)).unwrap_err(),
            CoreRpcClientError::RequestCancelled { request_id }
        );

        let deadline_error = client
            .request("status.inspect", 25, json!({}))
            .expect_err("held request must expire");
        assert!(matches!(
            deadline_error,
            CoreRpcClientError::DeadlineExpired { .. }
        ));
        assert_eq!(client.ping(77, 5_000).unwrap().nonce, 77);

        client
            .shutdown("cancel_deadline_test", Duration::from_secs(5))
            .unwrap();
        server.join();
    }
}
