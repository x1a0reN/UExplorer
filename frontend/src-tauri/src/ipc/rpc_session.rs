use serde::Serialize;
use serde_json::Value;
use std::collections::{BTreeMap, VecDeque};
use std::fmt;
use uexplorer_protocol::{
    encode_frame, CancelPayload, EventPayload, Frame, FrameDecoder, FrameKind, HeartbeatPayload,
    HelloPayload, ProtocolError, ProtocolLimits, ProtocolVersion, RequestPayload, ResponsePayload,
    ShutdownPayload, WelcomePayload, MAX_GENERATION, MAX_PAYLOAD_SIZE, PROTOCOL_MAJOR,
    PROTOCOL_MINOR,
};

const MAX_VERSION_BYTES: usize = 64;
const MAX_ENGINE_PROFILE_BYTES: usize = 128;
const MAX_SESSION_ID_BYTES: usize = 128;
const MAX_OPERATION_BYTES: usize = 128;
const MAX_REASON_BYTES: usize = 256;
const MAX_ERROR_CODE_BYTES: usize = 64;
const MAX_ERROR_MESSAGE_BYTES: usize = 1_024;
const MAX_RETIRED_REQUESTS: usize = 512;
const RETIRED_REQUEST_TTL_US: u64 = 30_000_000;
// Core uses timeout_ms as its execution deadline. Keep the transport request
// pending briefly afterward so an explicit unknown-mutation outcome is not
// retired as a generic client deadline before Core can serialize it.
const REQUEST_SETTLEMENT_GRACE_US: u64 = 1_000_000;
const MAX_RECEIVE_CHUNK_BYTES: usize = 64 * 1024;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RpcSessionState {
    Created,
    HelloSent,
    Ready,
    Closing,
    Closed,
    Failed,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum RpcSessionError {
    InvalidConfiguration(&'static str),
    InvalidState {
        operation: &'static str,
        state: RpcSessionState,
    },
    Protocol(String),
    InvalidJson(String),
    InvalidEnvelope(&'static str),
    PeerPidMismatch {
        expected: u32,
        actual: u32,
    },
    SessionMismatch,
    PendingLimitReached,
    RequestNotPending,
    RetiredLimitReached,
    RequestIdExhausted,
    DeadlineOverflow,
}

impl RpcSessionError {
    pub fn code(&self) -> &'static str {
        match self {
            Self::InvalidConfiguration(_) => "RPC_CONFIGURATION_INVALID",
            Self::InvalidState { .. } => "RPC_STATE_INVALID",
            Self::Protocol(_) => "RPC_PROTOCOL_ERROR",
            Self::InvalidJson(_) => "RPC_JSON_INVALID",
            Self::InvalidEnvelope(_) => "RPC_ENVELOPE_INVALID",
            Self::PeerPidMismatch { .. } => "RPC_PEER_PID_MISMATCH",
            Self::SessionMismatch => "RPC_SESSION_MISMATCH",
            Self::PendingLimitReached => "RPC_PENDING_LIMIT_REACHED",
            Self::RequestNotPending => "RPC_REQUEST_NOT_PENDING",
            Self::RetiredLimitReached => "RPC_RETIRED_LIMIT_REACHED",
            Self::RequestIdExhausted => "RPC_REQUEST_ID_EXHAUSTED",
            Self::DeadlineOverflow => "RPC_DEADLINE_OVERFLOW",
        }
    }
}

impl fmt::Display for RpcSessionError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidConfiguration(reason) => write!(formatter, "invalid RPC config: {reason}"),
            Self::InvalidState { operation, state } => {
                write!(
                    formatter,
                    "RPC operation {operation} is invalid in state {state:?}"
                )
            }
            Self::Protocol(reason) => write!(formatter, "RPC protocol error: {reason}"),
            Self::InvalidJson(reason) => write!(formatter, "invalid RPC JSON: {reason}"),
            Self::InvalidEnvelope(reason) => write!(formatter, "invalid RPC envelope: {reason}"),
            Self::PeerPidMismatch { expected, actual } => {
                write!(
                    formatter,
                    "RPC peer PID mismatch: expected {expected}, got {actual}"
                )
            }
            Self::SessionMismatch => write!(formatter, "RPC session ID mismatch"),
            Self::PendingLimitReached => write!(formatter, "RPC pending request limit reached"),
            Self::RequestNotPending => write!(formatter, "RPC request is not pending"),
            Self::RetiredLimitReached => write!(formatter, "RPC retired request limit reached"),
            Self::RequestIdExhausted => write!(formatter, "RPC request IDs are exhausted"),
            Self::DeadlineOverflow => write!(formatter, "RPC deadline overflow"),
        }
    }
}

impl std::error::Error for RpcSessionError {}

impl From<ProtocolError> for RpcSessionError {
    fn from(value: ProtocolError) -> Self {
        Self::Protocol(value.to_string())
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct OutboundFrame {
    pub kind: FrameKind,
    pub request_id: u64,
    pub bytes: Vec<u8>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum RpcInbound {
    Ready(WelcomePayload),
    Response(ResponsePayload),
    Event(EventPayload),
    Pong {
        request_id: u64,
        payload: HeartbeatPayload,
    },
    Reply(OutboundFrame),
    Shutdown(ShutdownPayload),
    LateResponse {
        request_id: u64,
    },
    LatePong {
        request_id: u64,
    },
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ExpiredRequest {
    pub request_id: u64,
    pub operation: Option<String>,
    pub cancel: Option<OutboundFrame>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
enum PendingKind {
    Request { operation: String },
    Ping { nonce: u64 },
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct PendingRequest {
    deadline_us: u64,
    timeout_ms: u32,
    kind: PendingKind,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum RetiredKind {
    Request,
    Ping,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct RetiredRequest {
    request_id: u64,
    expires_at_us: u64,
    kind: RetiredKind,
}

pub struct CoreRpcSession {
    target_pid: u32,
    host_version: String,
    state: RpcSessionState,
    decoder: FrameDecoder,
    next_request_id: u64,
    handshake_request_id: Option<u64>,
    shutdown_request_id: Option<u64>,
    shutdown_reason: Option<String>,
    welcome: Option<WelcomePayload>,
    pending: BTreeMap<u64, PendingRequest>,
    retired: VecDeque<RetiredRequest>,
    last_event_seq: u64,
    last_dropped_before: u64,
}

impl CoreRpcSession {
    pub fn new(target_pid: u32, host_version: impl Into<String>) -> Result<Self, RpcSessionError> {
        let host_version = host_version.into();
        if target_pid == 0 {
            return Err(RpcSessionError::InvalidConfiguration(
                "target PID must be positive",
            ));
        }
        if !is_bounded_text(&host_version, MAX_VERSION_BYTES) {
            return Err(RpcSessionError::InvalidConfiguration(
                "host version must be 1..64 bytes without control characters",
            ));
        }
        Ok(Self {
            target_pid,
            host_version,
            state: RpcSessionState::Created,
            decoder: FrameDecoder::default(),
            next_request_id: 1,
            handshake_request_id: None,
            shutdown_request_id: None,
            shutdown_reason: None,
            welcome: None,
            pending: BTreeMap::new(),
            retired: VecDeque::new(),
            last_event_seq: 0,
            last_dropped_before: 0,
        })
    }

    pub fn state(&self) -> RpcSessionState {
        self.state
    }

    pub fn welcome(&self) -> Option<&WelcomePayload> {
        self.welcome.as_ref()
    }

    pub fn pending_count(&self) -> usize {
        self.pending.len()
    }

    pub fn start_handshake(&mut self) -> Result<OutboundFrame, RpcSessionError> {
        self.require_state("start_handshake", &[RpcSessionState::Created])?;
        let request_id = self.allocate_request_id()?;
        let payload = HelloPayload {
            host_version: self.host_version.clone(),
            protocol: ProtocolVersion {
                major: PROTOCOL_MAJOR,
                minor: PROTOCOL_MINOR,
            },
            target_pid: self.target_pid,
        };
        let frame = self.serialize_frame(FrameKind::Hello, request_id, &payload)?;
        self.handshake_request_id = Some(request_id);
        self.state = RpcSessionState::HelloSent;
        Ok(frame)
    }

    pub fn start_request(
        &mut self,
        operation: impl Into<String>,
        timeout_ms: u32,
        data: Value,
        now_us: u64,
    ) -> Result<OutboundFrame, RpcSessionError> {
        self.require_state("start_request", &[RpcSessionState::Ready])?;
        let operation = operation.into();
        if !is_valid_operation(&operation) {
            return Err(RpcSessionError::InvalidConfiguration(
                "operation does not match the v1 operation grammar",
            ));
        }
        let (session_id, max_timeout_ms, pending_limit) = {
            let welcome = self.ready_welcome()?;
            (
                welcome.session_id.clone(),
                welcome.limits.max_timeout_ms,
                welcome.limits.pending_rpc_per_session as usize,
            )
        };
        if timeout_ms == 0 || timeout_ms > max_timeout_ms {
            return Err(RpcSessionError::InvalidConfiguration(
                "timeout is outside the negotiated limit",
            ));
        }
        if self.pending.len() >= pending_limit {
            return Err(RpcSessionError::PendingLimitReached);
        }
        let deadline_us = deadline_from_request_timeout(now_us, timeout_ms)?;
        let request_id = self.allocate_request_id()?;
        let payload = RequestPayload {
            operation: operation.clone(),
            session_id,
            timeout_ms,
            data,
        };
        let frame = self.serialize_frame(FrameKind::Request, request_id, &payload)?;
        self.pending.insert(
            request_id,
            PendingRequest {
                deadline_us,
                timeout_ms,
                kind: PendingKind::Request { operation },
            },
        );
        Ok(frame)
    }

    /// Rebase the local settlement deadline after the request frame is fully
    /// written. The peer cannot start its execution budget before this point.
    pub fn mark_request_sent(
        &mut self,
        request_id: u64,
        sent_at_us: u64,
    ) -> Result<(), RpcSessionError> {
        self.require_state("mark_request_sent", &[RpcSessionState::Ready])?;
        let pending = self
            .pending
            .get_mut(&request_id)
            .ok_or(RpcSessionError::RequestNotPending)?;
        if !matches!(pending.kind, PendingKind::Request { .. }) {
            return Err(RpcSessionError::RequestNotPending);
        }
        pending.deadline_us = deadline_from_request_timeout(sent_at_us, pending.timeout_ms)?;
        Ok(())
    }

    pub fn start_ping(
        &mut self,
        nonce: u64,
        timeout_ms: u32,
        now_us: u64,
    ) -> Result<OutboundFrame, RpcSessionError> {
        self.require_state("start_ping", &[RpcSessionState::Ready])?;
        if nonce == 0 || nonce > MAX_GENERATION || now_us == 0 {
            return Err(RpcSessionError::InvalidConfiguration(
                "ping nonce and monotonic timestamp must be positive protocol integers",
            ));
        }
        let (session_id, max_timeout_ms, pending_limit) = {
            let welcome = self.ready_welcome()?;
            (
                welcome.session_id.clone(),
                welcome.limits.max_timeout_ms,
                welcome.limits.pending_rpc_per_session as usize,
            )
        };
        if timeout_ms == 0 || timeout_ms > max_timeout_ms {
            return Err(RpcSessionError::InvalidConfiguration(
                "ping timeout is outside the negotiated limit",
            ));
        }
        if self.pending.len() >= pending_limit {
            return Err(RpcSessionError::PendingLimitReached);
        }
        let deadline_us = deadline_from_timeout(now_us, timeout_ms)?;
        let request_id = self.allocate_request_id()?;
        let payload = HeartbeatPayload {
            session_id,
            nonce,
            sent_at_monotonic_us: now_us,
        };
        let frame = self.serialize_frame(FrameKind::Ping, request_id, &payload)?;
        self.pending.insert(
            request_id,
            PendingRequest {
                deadline_us,
                timeout_ms,
                kind: PendingKind::Ping { nonce },
            },
        );
        Ok(frame)
    }

    pub fn cancel_request(
        &mut self,
        request_id: u64,
        reason: impl Into<String>,
        now_us: u64,
    ) -> Result<OutboundFrame, RpcSessionError> {
        self.require_state("cancel_request", &[RpcSessionState::Ready])?;
        let reason = reason.into();
        if !is_bounded_text(&reason, MAX_REASON_BYTES) {
            return Err(RpcSessionError::InvalidConfiguration(
                "cancel reason must be 1..256 bytes without control characters",
            ));
        }
        self.prune_retired(now_us);
        if self.retired.len() >= MAX_RETIRED_REQUESTS {
            return Err(RpcSessionError::RetiredLimitReached);
        }
        let expires_at_us = retirement_expiry(now_us)?;
        let pending = self
            .pending
            .get(&request_id)
            .ok_or(RpcSessionError::RequestNotPending)?;
        if !matches!(pending.kind, PendingKind::Request { .. }) {
            return Err(RpcSessionError::RequestNotPending);
        }
        let payload = CancelPayload {
            session_id: self.ready_welcome()?.session_id.clone(),
            reason,
        };
        let frame = self.serialize_frame(FrameKind::Cancel, request_id, &payload)?;
        self.retire(request_id, RetiredKind::Request, expires_at_us);
        self.pending.remove(&request_id);
        Ok(frame)
    }

    pub fn expire_requests(&mut self, now_us: u64) -> Result<Vec<ExpiredRequest>, RpcSessionError> {
        self.require_state(
            "expire_requests",
            &[RpcSessionState::Ready, RpcSessionState::Closing],
        )?;
        self.prune_retired(now_us);
        let expired_ids: Vec<u64> = self
            .pending
            .iter()
            .filter_map(|(request_id, pending)| {
                (pending.deadline_us <= now_us).then_some(*request_id)
            })
            .collect();
        if self.retired.len() + expired_ids.len() > MAX_RETIRED_REQUESTS {
            return Err(RpcSessionError::RetiredLimitReached);
        }
        let expires_at_us = if expired_ids.is_empty() {
            None
        } else {
            Some(retirement_expiry(now_us)?)
        };

        let mut expired = Vec::with_capacity(expired_ids.len());
        for request_id in expired_ids {
            let pending = self
                .pending
                .get(&request_id)
                .cloned()
                .ok_or(RpcSessionError::RequestNotPending)?;
            let (operation, cancel, retired_kind) = match pending.kind {
                PendingKind::Request { operation } => {
                    let payload = CancelPayload {
                        session_id: self.ready_welcome()?.session_id.clone(),
                        reason: "deadline_expired".to_string(),
                    };
                    (
                        Some(operation),
                        Some(self.serialize_frame(FrameKind::Cancel, request_id, &payload)?),
                        RetiredKind::Request,
                    )
                }
                PendingKind::Ping { .. } => (None, None, RetiredKind::Ping),
            };
            self.retire(
                request_id,
                retired_kind,
                expires_at_us.expect("nonempty expiry batch"),
            );
            self.pending.remove(&request_id);
            expired.push(ExpiredRequest {
                request_id,
                operation,
                cancel,
            });
        }
        Ok(expired)
    }

    pub fn start_shutdown(
        &mut self,
        reason: impl Into<String>,
    ) -> Result<OutboundFrame, RpcSessionError> {
        self.require_state("start_shutdown", &[RpcSessionState::Ready])?;
        let reason = reason.into();
        if !is_bounded_text(&reason, MAX_REASON_BYTES) {
            return Err(RpcSessionError::InvalidConfiguration(
                "shutdown reason must be 1..256 bytes without control characters",
            ));
        }
        let session_id = self.ready_welcome()?.session_id.clone();
        let request_id = self.allocate_request_id()?;
        let shutdown_reason = reason.clone();
        let payload = ShutdownPayload { session_id, reason };
        let frame = self.serialize_frame(FrameKind::Shutdown, request_id, &payload)?;
        self.shutdown_request_id = Some(request_id);
        self.shutdown_reason = Some(shutdown_reason);
        self.state = RpcSessionState::Closing;
        Ok(frame)
    }

    pub fn receive(&mut self, bytes: &[u8]) -> Result<Vec<RpcInbound>, RpcSessionError> {
        if matches!(
            self.state,
            RpcSessionState::Closed | RpcSessionState::Failed
        ) {
            return Err(RpcSessionError::InvalidState {
                operation: "receive",
                state: self.state,
            });
        }
        if bytes.len() > MAX_RECEIVE_CHUNK_BYTES {
            return self.fail(RpcSessionError::InvalidEnvelope(
                "transport read chunk exceeds the Host receive bound",
            ));
        }
        let frames = match self.decoder.push(bytes) {
            Ok(frames) => frames,
            Err(error) => return self.fail(error.into()),
        };
        let mut inbound = Vec::with_capacity(frames.len());
        for frame in frames {
            match self.handle_frame(frame) {
                Ok(message) => inbound.push(message),
                Err(error) => return self.fail(error),
            }
        }
        Ok(inbound)
    }

    pub fn disconnect(&mut self) {
        self.pending.clear();
        self.retired.clear();
        self.handshake_request_id = None;
        self.shutdown_request_id = None;
        self.shutdown_reason = None;
        self.welcome = None;
        self.state = RpcSessionState::Closed;
    }

    fn handle_frame(&mut self, frame: Frame) -> Result<RpcInbound, RpcSessionError> {
        if self.state == RpcSessionState::HelloSent {
            return self.handle_welcome(frame);
        }
        self.require_state(
            "handle_frame",
            &[RpcSessionState::Ready, RpcSessionState::Closing],
        )?;
        if frame.header.minor != PROTOCOL_MINOR {
            return Err(RpcSessionError::InvalidEnvelope(
                "frame minor version differs from the negotiated session version",
            ));
        }
        if frame.header.payload_len > self.ready_welcome()?.limits.max_payload_bytes {
            return Err(RpcSessionError::InvalidEnvelope(
                "peer payload exceeds the negotiated frame limit",
            ));
        }
        match frame.header.kind {
            FrameKind::Response => self.handle_response(frame),
            FrameKind::Event => self.handle_event(frame),
            FrameKind::Ping => self.handle_ping(frame),
            FrameKind::Pong => self.handle_pong(frame),
            FrameKind::Shutdown => self.handle_shutdown(frame),
            _ => Err(RpcSessionError::InvalidEnvelope(
                "peer sent a frame kind that is invalid for a Host session",
            )),
        }
    }

    fn handle_welcome(&mut self, frame: Frame) -> Result<RpcInbound, RpcSessionError> {
        if frame.header.kind != FrameKind::Welcome
            || frame.header.request_id == 0
            || Some(frame.header.request_id) != self.handshake_request_id
        {
            return Err(RpcSessionError::InvalidEnvelope(
                "handshake expected one correlated Welcome frame",
            ));
        }
        let welcome: WelcomePayload = deserialize_payload(&frame.payload)?;
        validate_welcome(self.target_pid, frame.header.minor, &welcome)?;
        self.decoder
            .set_payload_limit(welcome.limits.max_payload_bytes)?;
        self.welcome = Some(welcome.clone());
        self.handshake_request_id = None;
        self.state = RpcSessionState::Ready;
        Ok(RpcInbound::Ready(welcome))
    }

    fn handle_response(&mut self, frame: Frame) -> Result<RpcInbound, RpcSessionError> {
        if frame.header.request_id == 0 {
            return Err(RpcSessionError::InvalidEnvelope(
                "Response request ID must be positive",
            ));
        }
        let response: ResponsePayload = deserialize_payload(&frame.payload)?;
        if response.request_id != frame.header.request_id {
            return Err(RpcSessionError::InvalidEnvelope(
                "Response payload and frame request IDs differ",
            ));
        }
        self.validate_session(&response.session_id)?;
        validate_response(&response)?;

        if let Some(pending) = self.pending.get(&frame.header.request_id) {
            if !matches!(pending.kind, PendingKind::Request { .. }) {
                return Err(RpcSessionError::InvalidEnvelope(
                    "Response correlated to a pending Ping",
                ));
            }
            self.pending.remove(&frame.header.request_id);
            return Ok(RpcInbound::Response(response));
        }
        if self.remove_retired(frame.header.request_id, RetiredKind::Request) {
            return Ok(RpcInbound::LateResponse {
                request_id: frame.header.request_id,
            });
        }
        Err(RpcSessionError::InvalidEnvelope(
            "Response has no pending or retired correlation",
        ))
    }

    fn handle_event(&mut self, frame: Frame) -> Result<RpcInbound, RpcSessionError> {
        if frame.header.request_id != 0 {
            return Err(RpcSessionError::InvalidEnvelope(
                "Event request ID must be zero",
            ));
        }
        let event: EventPayload = deserialize_payload(&frame.payload)?;
        self.validate_session(&event.session_id)?;
        if event.seq == 0
            || event.seq > MAX_GENERATION
            || event.seq <= self.last_event_seq
            || event.timestamp_us > MAX_GENERATION
            || event.dropped_before > MAX_GENERATION
            || event.dropped_before >= event.seq
            || event.dropped_before < self.last_dropped_before
            || !is_valid_operation(&event.kind)
        {
            return Err(RpcSessionError::InvalidEnvelope(
                "Event sequence, drop counter, or kind is invalid",
            ));
        }
        if self.last_event_seq != 0 && event.seq > self.last_event_seq + 1 {
            let missing = event.seq - self.last_event_seq - 1;
            if event.dropped_before < self.last_dropped_before.saturating_add(missing) {
                return Err(RpcSessionError::InvalidEnvelope(
                    "Event sequence gap is not accounted for by the drop counter",
                ));
            }
        }
        self.last_event_seq = event.seq;
        self.last_dropped_before = event.dropped_before;
        Ok(RpcInbound::Event(event))
    }

    fn handle_ping(&self, frame: Frame) -> Result<RpcInbound, RpcSessionError> {
        if frame.header.request_id == 0 {
            return Err(RpcSessionError::InvalidEnvelope(
                "Ping request ID must be positive",
            ));
        }
        let heartbeat: HeartbeatPayload = deserialize_payload(&frame.payload)?;
        self.validate_heartbeat(&heartbeat)?;
        Ok(RpcInbound::Reply(self.serialize_frame(
            FrameKind::Pong,
            frame.header.request_id,
            &heartbeat,
        )?))
    }

    fn handle_pong(&mut self, frame: Frame) -> Result<RpcInbound, RpcSessionError> {
        if frame.header.request_id == 0 {
            return Err(RpcSessionError::InvalidEnvelope(
                "Pong request ID must be positive",
            ));
        }
        let heartbeat: HeartbeatPayload = deserialize_payload(&frame.payload)?;
        self.validate_heartbeat(&heartbeat)?;
        if let Some(pending) = self.pending.get(&frame.header.request_id) {
            match pending.kind {
                PendingKind::Ping { nonce } if nonce == heartbeat.nonce => {}
                _ => {
                    return Err(RpcSessionError::InvalidEnvelope(
                        "Pong nonce or pending request kind is invalid",
                    ));
                }
            }
            self.pending.remove(&frame.header.request_id);
            return Ok(RpcInbound::Pong {
                request_id: frame.header.request_id,
                payload: heartbeat,
            });
        }
        if self.remove_retired(frame.header.request_id, RetiredKind::Ping) {
            return Ok(RpcInbound::LatePong {
                request_id: frame.header.request_id,
            });
        }
        Err(RpcSessionError::InvalidEnvelope(
            "Pong has no pending or retired correlation",
        ))
    }

    fn handle_shutdown(&mut self, frame: Frame) -> Result<RpcInbound, RpcSessionError> {
        if frame.header.request_id == 0 {
            return Err(RpcSessionError::InvalidEnvelope(
                "Shutdown request ID must be positive",
            ));
        }
        let shutdown: ShutdownPayload = deserialize_payload(&frame.payload)?;
        self.validate_session(&shutdown.session_id)?;
        if self.state == RpcSessionState::Closing
            && (self.shutdown_request_id != Some(frame.header.request_id)
                || self.shutdown_reason.as_deref() != Some(shutdown.reason.as_str()))
        {
            return Err(RpcSessionError::InvalidEnvelope(
                "Shutdown acknowledgement does not match the request",
            ));
        }
        if !is_bounded_text(&shutdown.reason, MAX_REASON_BYTES) {
            return Err(RpcSessionError::InvalidEnvelope(
                "Shutdown reason is invalid",
            ));
        }
        self.pending.clear();
        self.retired.clear();
        self.shutdown_request_id = None;
        self.shutdown_reason = None;
        self.state = RpcSessionState::Closed;
        Ok(RpcInbound::Shutdown(shutdown))
    }

    fn validate_heartbeat(&self, heartbeat: &HeartbeatPayload) -> Result<(), RpcSessionError> {
        self.validate_session(&heartbeat.session_id)?;
        if heartbeat.nonce == 0
            || heartbeat.nonce > MAX_GENERATION
            || heartbeat.sent_at_monotonic_us == 0
        {
            return Err(RpcSessionError::InvalidEnvelope(
                "Heartbeat nonce or timestamp is invalid",
            ));
        }
        Ok(())
    }

    fn validate_session(&self, session_id: &str) -> Result<(), RpcSessionError> {
        if self.ready_welcome()?.session_id != session_id {
            return Err(RpcSessionError::SessionMismatch);
        }
        Ok(())
    }

    fn ready_welcome(&self) -> Result<&WelcomePayload, RpcSessionError> {
        self.welcome.as_ref().ok_or(RpcSessionError::InvalidState {
            operation: "read_welcome",
            state: self.state,
        })
    }

    fn serialize_frame<T: Serialize>(
        &self,
        kind: FrameKind,
        request_id: u64,
        payload: &T,
    ) -> Result<OutboundFrame, RpcSessionError> {
        if request_id == 0 && kind != FrameKind::Event {
            return Err(RpcSessionError::InvalidEnvelope(
                "correlated frame request ID must be positive",
            ));
        }
        let payload = serde_json::to_vec(payload)
            .map_err(|error| RpcSessionError::InvalidJson(error.to_string()))?;
        let negotiated_limit = self
            .welcome
            .as_ref()
            .map_or(MAX_PAYLOAD_SIZE, |welcome| welcome.limits.max_payload_bytes);
        if payload.len() > negotiated_limit as usize {
            return Err(RpcSessionError::InvalidEnvelope(
                "payload exceeds the negotiated frame limit",
            ));
        }
        let bytes = encode_frame(kind, request_id, &payload)?;
        Ok(OutboundFrame {
            kind,
            request_id,
            bytes,
        })
    }

    fn allocate_request_id(&mut self) -> Result<u64, RpcSessionError> {
        if self.next_request_id == 0 || self.next_request_id > MAX_GENERATION {
            return Err(RpcSessionError::RequestIdExhausted);
        }
        let request_id = self.next_request_id;
        self.next_request_id = if request_id == MAX_GENERATION {
            0
        } else {
            request_id + 1
        };
        Ok(request_id)
    }

    fn require_state(
        &self,
        operation: &'static str,
        expected: &[RpcSessionState],
    ) -> Result<(), RpcSessionError> {
        if !expected.contains(&self.state) {
            return Err(RpcSessionError::InvalidState {
                operation,
                state: self.state,
            });
        }
        Ok(())
    }

    fn retire(&mut self, request_id: u64, kind: RetiredKind, expires_at_us: u64) {
        self.retired.push_back(RetiredRequest {
            request_id,
            expires_at_us,
            kind,
        });
    }

    fn prune_retired(&mut self, now_us: u64) {
        self.retired
            .retain(|request| request.expires_at_us > now_us);
    }

    fn remove_retired(&mut self, request_id: u64, kind: RetiredKind) -> bool {
        let position = self
            .retired
            .iter()
            .position(|request| request.request_id == request_id && request.kind == kind);
        position
            .and_then(|position| self.retired.remove(position))
            .is_some()
    }

    fn fail<T>(&mut self, error: RpcSessionError) -> Result<T, RpcSessionError> {
        self.pending.clear();
        self.retired.clear();
        self.handshake_request_id = None;
        self.shutdown_request_id = None;
        self.shutdown_reason = None;
        self.state = RpcSessionState::Failed;
        Err(error)
    }
}

fn deserialize_payload<T>(payload: &[u8]) -> Result<T, RpcSessionError>
where
    T: serde::de::DeserializeOwned,
{
    serde_json::from_slice(payload).map_err(|error| RpcSessionError::InvalidJson(error.to_string()))
}

fn validate_welcome(
    expected_pid: u32,
    frame_minor: u16,
    welcome: &WelcomePayload,
) -> Result<(), RpcSessionError> {
    if welcome.target_pid != expected_pid {
        return Err(RpcSessionError::PeerPidMismatch {
            expected: expected_pid,
            actual: welcome.target_pid,
        });
    }
    if welcome.protocol.major != PROTOCOL_MAJOR
        || welcome.protocol.minor != PROTOCOL_MINOR
        || frame_minor != welcome.protocol.minor
    {
        return Err(RpcSessionError::InvalidEnvelope(
            "Welcome did not select the exact offered protocol version",
        ));
    }
    if !is_bounded_text(&welcome.core_version, MAX_VERSION_BYTES)
        || !is_valid_session_id(&welcome.session_id)
        || welcome
            .engine_profile
            .as_ref()
            .is_some_and(|profile| !is_bounded_text(profile, MAX_ENGINE_PROFILE_BYTES))
        || welcome.capabilities.is_empty()
        || welcome
            .capabilities
            .keys()
            .any(|capability| !is_valid_operation(capability))
        || welcome.capabilities.get("engine.core") != Some(&true)
        || welcome.capabilities.get("transport.named_pipe") != Some(&true)
    {
        return Err(RpcSessionError::InvalidEnvelope(
            "Welcome identity, profile, or required capabilities are invalid",
        ));
    }
    validate_limits(&welcome.limits)
}

fn validate_limits(limits: &ProtocolLimits) -> Result<(), RpcSessionError> {
    if limits.max_payload_bytes == 0
        || limits.max_payload_bytes > MAX_PAYLOAD_SIZE
        || limits.pending_rpc_per_session == 0
        || limits.pending_rpc_per_session > 256
        || limits.game_thread_tasks == 0
        || limits.game_thread_tasks > 128
        || limits.hook_event_ring == 0
        || limits.hook_event_ring > 8_192
        || limits.subscriber_events == 0
        || limits.subscriber_events > 1_024
        || limits.dump_running != 1
        || limits.max_timeout_ms == 0
        || limits.max_timeout_ms > 120_000
    {
        return Err(RpcSessionError::InvalidEnvelope(
            "Welcome limits exceed the v1 safety contract",
        ));
    }
    Ok(())
}

fn validate_response(response: &ResponsePayload) -> Result<(), RpcSessionError> {
    if response.request_id == 0
        || (response.ok && response.error.is_some())
        || (!response.ok && (response.error.is_none() || !response.data.is_null()))
    {
        return Err(RpcSessionError::InvalidEnvelope(
            "Response success/error/data invariants are invalid",
        ));
    }
    if let Some(error) = &response.error {
        if !is_valid_error_code(&error.code)
            || error.message.is_empty()
            || error.message.len() > MAX_ERROR_MESSAGE_BYTES
            || error.message.chars().any(char::is_control)
            || !error.details.is_object()
        {
            return Err(RpcSessionError::InvalidEnvelope(
                "Response error payload is invalid",
            ));
        }
    }
    Ok(())
}

fn deadline_from_timeout(now_us: u64, timeout_ms: u32) -> Result<u64, RpcSessionError> {
    now_us
        .checked_add(u64::from(timeout_ms) * 1_000)
        .ok_or(RpcSessionError::DeadlineOverflow)
}

fn deadline_from_request_timeout(now_us: u64, timeout_ms: u32) -> Result<u64, RpcSessionError> {
    deadline_from_timeout(now_us, timeout_ms)?
        .checked_add(REQUEST_SETTLEMENT_GRACE_US)
        .ok_or(RpcSessionError::DeadlineOverflow)
}

fn retirement_expiry(now_us: u64) -> Result<u64, RpcSessionError> {
    now_us
        .checked_add(RETIRED_REQUEST_TTL_US)
        .ok_or(RpcSessionError::DeadlineOverflow)
}

fn is_valid_operation(value: &str) -> bool {
    if value.is_empty() || value.len() > MAX_OPERATION_BYTES {
        return false;
    }
    value.bytes().enumerate().all(|(index, byte)| {
        if index == 0 {
            return byte.is_ascii_lowercase();
        }
        byte.is_ascii_lowercase()
            || byte.is_ascii_digit()
            || byte == b'_'
            || byte == b'-'
            || byte == b'.'
    })
}

fn is_valid_error_code(value: &str) -> bool {
    if value.is_empty() || value.len() > MAX_ERROR_CODE_BYTES {
        return false;
    }
    value.bytes().enumerate().all(|(index, byte)| {
        if index == 0 {
            return byte.is_ascii_uppercase();
        }
        byte.is_ascii_uppercase() || byte.is_ascii_digit() || byte == b'_'
    })
}

fn is_valid_session_id(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= MAX_SESSION_ID_BYTES
        && value
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'-' || byte == b'_')
}

fn is_bounded_text(value: &str, maximum: usize) -> bool {
    !value.is_empty() && value.len() <= maximum && !value.chars().any(char::is_control)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use uexplorer_fake_core::FakeCore;
    use uexplorer_protocol::{decode_header, RpcErrorPayload, RpcTiming};

    fn fixture_welcome() -> WelcomePayload {
        serde_json::from_str(include_str!(
            "../../../../protocol/v1/fixtures/welcome.json"
        ))
        .unwrap()
    }

    fn receive_welcome(
        mut welcome_payload: WelcomePayload,
    ) -> Result<CoreRpcSession, RpcSessionError> {
        let mut session = CoreRpcSession::new(4242, "test-host").unwrap();
        let hello = session.start_handshake().unwrap();
        welcome_payload.target_pid = 4242;
        let welcome = encode_frame(
            FrameKind::Welcome,
            hello.request_id,
            &serde_json::to_vec(&welcome_payload).unwrap(),
        )
        .unwrap();
        let mut messages = Vec::new();
        for byte in welcome {
            messages.extend(session.receive(&[byte])?);
        }
        assert!(matches!(messages.as_slice(), [RpcInbound::Ready(_)]));
        Ok(session)
    }

    fn ready_session() -> CoreRpcSession {
        receive_welcome(fixture_welcome()).unwrap()
    }

    fn response_frame(request_id: u64, response: &ResponsePayload) -> Vec<u8> {
        encode_frame(
            FrameKind::Response,
            request_id,
            &serde_json::to_vec(response).unwrap(),
        )
        .unwrap()
    }

    #[test]
    fn one_byte_welcome_requires_exact_pid_capabilities_and_limits() {
        let session = ready_session();
        assert_eq!(session.state(), RpcSessionState::Ready);
        let welcome = session.welcome().unwrap();
        assert_eq!(welcome.target_pid, 4242);
        assert_eq!(welcome.limits.pending_rpc_per_session, 256);
        assert!(welcome.capabilities["transport.named_pipe"]);
    }

    #[test]
    fn invalid_welcome_identity_capability_and_limits_are_terminal() {
        let mut session = CoreRpcSession::new(4242, "test-host").unwrap();
        let hello = session.start_handshake().unwrap();
        let mut wrong_pid = fixture_welcome();
        wrong_pid.target_pid = 7777;
        let frame = encode_frame(
            FrameKind::Welcome,
            hello.request_id,
            &serde_json::to_vec(&wrong_pid).unwrap(),
        )
        .unwrap();
        assert!(matches!(
            session.receive(&frame),
            Err(RpcSessionError::PeerPidMismatch {
                expected: 4242,
                actual: 7777
            })
        ));
        assert_eq!(session.state(), RpcSessionState::Failed);

        let mut missing_transport = fixture_welcome();
        missing_transport
            .capabilities
            .remove("transport.named_pipe");
        assert!(receive_welcome(missing_transport).is_err());

        let mut excessive_limit = fixture_welcome();
        excessive_limit.limits.pending_rpc_per_session = 257;
        assert!(receive_welcome(excessive_limit).is_err());
    }

    #[test]
    fn strict_request_response_correlation_and_envelope_are_enforced() {
        let mut session = ready_session();
        let request = session
            .start_request("status.inspect", 5_000, json!({}), 1_000_000)
            .unwrap();
        assert_eq!(
            decode_header(&request.bytes).unwrap().kind,
            FrameKind::Request
        );
        assert_eq!(session.pending_count(), 1);

        let response = ResponsePayload {
            ok: true,
            request_id: request.request_id,
            session_id: "fixture-session-4242".to_string(),
            error: None,
            data: json!({"state": "ready"}),
            timing: RpcTiming {
                queued_us: 0,
                execute_us: 5,
            },
        };
        let inbound = session
            .receive(&response_frame(request.request_id, &response))
            .unwrap();
        assert!(matches!(inbound.as_slice(), [RpcInbound::Response(value)] if value.ok));
        assert_eq!(session.pending_count(), 0);

        let unknown = response_frame(
            request.request_id + 100,
            &ResponsePayload {
                request_id: request.request_id + 100,
                ..response
            },
        );
        assert!(session.receive(&unknown).is_err());
        assert_eq!(session.state(), RpcSessionState::Failed);
    }

    #[test]
    fn cancellation_and_deadline_keep_bounded_late_response_tombstones() {
        let mut session = ready_session();
        let request = session
            .start_request("objects.snapshot.page", 5_000, json!({}), 1_000_000)
            .unwrap();
        let cancel = session
            .cancel_request(request.request_id, "caller_cancelled", 1_100_000)
            .unwrap();
        assert_eq!(cancel.kind, FrameKind::Cancel);
        assert_eq!(session.pending_count(), 0);

        let late = ResponsePayload {
            ok: false,
            request_id: request.request_id,
            session_id: "fixture-session-4242".to_string(),
            error: Some(RpcErrorPayload {
                code: "REQUEST_CANCELLED".to_string(),
                message: "cancelled".to_string(),
                details: json!({}),
            }),
            data: Value::Null,
            timing: RpcTiming {
                queued_us: 0,
                execute_us: 0,
            },
        };
        assert_eq!(
            session
                .receive(&response_frame(request.request_id, &late))
                .unwrap(),
            vec![RpcInbound::LateResponse {
                request_id: request.request_id
            }]
        );

        let expiring = session
            .start_request("status.health", 1, json!({}), 2_000_000)
            .unwrap();
        assert!(session.expire_requests(2_001_000).unwrap().is_empty());
        session
            .mark_request_sent(expiring.request_id, 2_500_000)
            .unwrap();
        assert!(session.expire_requests(3_001_000).unwrap().is_empty());
        let expired = session.expire_requests(3_501_000).unwrap();
        assert_eq!(expired.len(), 1);
        assert_eq!(expired[0].request_id, expiring.request_id);
        assert_eq!(expired[0].cancel.as_ref().unwrap().kind, FrameKind::Cancel);
    }

    #[test]
    fn ping_and_event_obey_session_and_sequence_boundaries() {
        let mut session = ready_session();
        let ping = session.start_ping(7, 1_000, 1_000_000).unwrap();
        let heartbeat: HeartbeatPayload = serde_json::from_str(include_str!(
            "../../../../protocol/v1/fixtures/heartbeat.json"
        ))
        .unwrap();
        let pong = encode_frame(
            FrameKind::Pong,
            ping.request_id,
            &serde_json::to_vec(&heartbeat).unwrap(),
        )
        .unwrap();
        assert!(matches!(
            session.receive(&pong).unwrap().as_slice(),
            [RpcInbound::Pong { request_id, payload }] if *request_id == ping.request_id && payload.nonce == 7
        ));

        let peer_ping = encode_frame(
            FrameKind::Ping,
            99,
            &serde_json::to_vec(&heartbeat).unwrap(),
        )
        .unwrap();
        assert!(matches!(
            session.receive(&peer_ping).unwrap().as_slice(),
            [RpcInbound::Reply(reply)] if reply.kind == FrameKind::Pong && reply.request_id == 99
        ));

        let event = EventPayload {
            seq: 10,
            kind: "snapshot.published".to_string(),
            timestamp_us: 2_000_000,
            session_id: "fixture-session-4242".to_string(),
            dropped_before: 2,
            data: json!({"generation": 9}),
        };
        let encoded_event =
            encode_frame(FrameKind::Event, 0, &serde_json::to_vec(&event).unwrap()).unwrap();
        assert!(matches!(
            session.receive(&encoded_event).unwrap().as_slice(),
            [RpcInbound::Event(value)] if value.seq == 10
        ));
        assert!(session.receive(&encoded_event).is_err());
        assert_eq!(session.state(), RpcSessionState::Failed);

        let mut gap_session = ready_session();
        gap_session.receive(&encoded_event).unwrap();
        let unaccounted_gap = EventPayload { seq: 12, ..event };
        let unaccounted_gap = encode_frame(
            FrameKind::Event,
            0,
            &serde_json::to_vec(&unaccounted_gap).unwrap(),
        )
        .unwrap();
        assert!(gap_session.receive(&unaccounted_gap).is_err());
        assert_eq!(gap_session.state(), RpcSessionState::Failed);
    }

    #[test]
    fn shutdown_requires_exact_acknowledgement_and_closes_the_session() {
        let mut session = ready_session();
        let outbound = session.start_shutdown("host_exit").unwrap();
        assert_eq!(session.state(), RpcSessionState::Closing);

        let shutdown: ShutdownPayload = serde_json::from_str(include_str!(
            "../../../../protocol/v1/fixtures/shutdown.json"
        ))
        .unwrap();
        let acknowledgement = encode_frame(
            FrameKind::Shutdown,
            outbound.request_id,
            &serde_json::to_vec(&shutdown).unwrap(),
        )
        .unwrap();
        assert!(matches!(
            session.receive(&acknowledgement).unwrap().as_slice(),
            [RpcInbound::Shutdown(value)] if value.reason == "host_exit"
        ));
        assert_eq!(session.state(), RpcSessionState::Closed);

        let mut wrong_id = ready_session();
        let outbound = wrong_id.start_shutdown("host_exit").unwrap();
        let wrong_acknowledgement = encode_frame(
            FrameKind::Shutdown,
            outbound.request_id + 1,
            &serde_json::to_vec(&shutdown).unwrap(),
        )
        .unwrap();
        assert!(wrong_id.receive(&wrong_acknowledgement).is_err());
        assert_eq!(wrong_id.state(), RpcSessionState::Failed);
    }

    #[test]
    fn negotiated_payload_and_minor_version_are_enforced_after_handshake() {
        let mut small_limit = fixture_welcome();
        small_limit.limits.max_payload_bytes = 64;
        let mut session = receive_welcome(small_limit).unwrap();
        let oversized_payload = vec![b' '; 65];
        let oversized = encode_frame(FrameKind::Event, 0, &oversized_payload).unwrap();
        assert!(session.receive(&oversized).is_err());
        assert_eq!(session.state(), RpcSessionState::Failed);

        let mut session = ready_session();
        let oversized_read = vec![0; MAX_RECEIVE_CHUNK_BYTES + 1];
        assert!(session.receive(&oversized_read).is_err());
        assert_eq!(session.state(), RpcSessionState::Failed);

        let mut session = ready_session();
        let event = EventPayload {
            seq: 1,
            kind: "runtime.ready".to_string(),
            timestamp_us: 1,
            session_id: "fixture-session-4242".to_string(),
            dropped_before: 0,
            data: json!({}),
        };
        let mut wrong_minor =
            encode_frame(FrameKind::Event, 0, &serde_json::to_vec(&event).unwrap()).unwrap();
        wrong_minor[6..8].copy_from_slice(&1u16.to_le_bytes());
        assert!(session.receive(&wrong_minor).is_err());
        assert_eq!(session.state(), RpcSessionState::Failed);
    }

    #[test]
    fn invalid_failure_envelope_is_terminal() {
        let mut session = ready_session();
        let request = session
            .start_request("status.inspect", 5_000, json!({}), 1_000_000)
            .unwrap();
        let invalid = ResponsePayload {
            ok: false,
            request_id: request.request_id,
            session_id: "fixture-session-4242".to_string(),
            error: None,
            data: json!({"not": "null"}),
            timing: RpcTiming {
                queued_us: 0,
                execute_us: 0,
            },
        };
        assert!(session
            .receive(&response_frame(request.request_id, &invalid))
            .is_err());
        assert_eq!(session.state(), RpcSessionState::Failed);
        assert_eq!(session.pending_count(), 0);
    }

    #[test]
    fn fake_core_drives_the_complete_host_rpc_lifecycle_contract() {
        let mut core = FakeCore::new(4242);
        let mut session = CoreRpcSession::new(4242, "test-host").unwrap();

        let hello = session.start_handshake().unwrap();
        let welcome = core.accept(&hello.bytes).unwrap();
        assert!(matches!(
            session.receive(&welcome[0]).unwrap().as_slice(),
            [RpcInbound::Ready(value)] if value.target_pid == 4242
        ));

        let request = session
            .start_request("status.inspect", 5_000, json!({}), 1_000_000)
            .unwrap();
        let response = core.accept(&request.bytes).unwrap();
        assert!(matches!(
            session.receive(&response[0]).unwrap().as_slice(),
            [RpcInbound::Response(value)] if value.ok && value.data["state"] == "ready"
        ));

        let ping = session.start_ping(7, 1_000, 2_000_000).unwrap();
        let pong = core.accept(&ping.bytes).unwrap();
        assert!(matches!(
            session.receive(&pong[0]).unwrap().as_slice(),
            [RpcInbound::Pong { request_id, payload }] if *request_id == ping.request_id && payload.nonce == 7
        ));

        let cancellable = session
            .start_request("objects.snapshot.page", 5_000, json!({}), 3_000_000)
            .unwrap();
        let cancel = session
            .cancel_request(cancellable.request_id, "caller_cancelled", 3_100_000)
            .unwrap();
        let cancelled = core.accept(&cancel.bytes).unwrap();
        assert_eq!(
            session.receive(&cancelled[0]).unwrap(),
            vec![RpcInbound::LateResponse {
                request_id: cancellable.request_id
            }]
        );

        let shutdown = session.start_shutdown("host_exit").unwrap();
        let acknowledgement = core.accept(&shutdown.bytes).unwrap();
        assert!(matches!(
            session.receive(&acknowledgement[0]).unwrap().as_slice(),
            [RpcInbound::Shutdown(value)] if value.reason == "host_exit"
        ));
        assert_eq!(session.state(), RpcSessionState::Closed);
    }
}
