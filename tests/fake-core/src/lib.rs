use serde_json::{json, Value};
use std::collections::BTreeMap;
use std::fmt;
use uexplorer_protocol::{
    encode_frame, CancelPayload, Frame, FrameDecoder, FrameKind, HeartbeatPayload, HelloPayload,
    ProtocolError, ProtocolLimits, ProtocolVersion, RequestPayload, ResponsePayload,
    RpcErrorPayload, RpcTiming, ShutdownPayload, WelcomePayload, MAX_PAYLOAD_SIZE, PROTOCOL_MAJOR,
    PROTOCOL_MINOR,
};

#[derive(Debug)]
pub enum FakeCoreError {
    Protocol(ProtocolError),
    InvalidJson(serde_json::Error),
    UnexpectedFrame(FrameKind),
    HandshakeRequired,
    DuplicateHandshake,
    TargetPidMismatch { expected: u32, actual: u32 },
    SessionMismatch,
    InvalidEnvelope(&'static str),
}

impl fmt::Display for FakeCoreError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Protocol(error) => write!(formatter, "protocol error: {error}"),
            Self::InvalidJson(error) => write!(formatter, "invalid JSON payload: {error}"),
            Self::UnexpectedFrame(kind) => write!(formatter, "unexpected frame: {kind:?}"),
            Self::HandshakeRequired => write!(formatter, "request received before handshake"),
            Self::DuplicateHandshake => write!(formatter, "duplicate handshake"),
            Self::TargetPidMismatch { expected, actual } => {
                write!(
                    formatter,
                    "target PID mismatch: expected {expected}, got {actual}"
                )
            }
            Self::SessionMismatch => write!(formatter, "session ID mismatch"),
            Self::InvalidEnvelope(reason) => write!(formatter, "invalid envelope: {reason}"),
        }
    }
}

impl std::error::Error for FakeCoreError {}

impl From<ProtocolError> for FakeCoreError {
    fn from(value: ProtocolError) -> Self {
        Self::Protocol(value)
    }
}

impl From<serde_json::Error> for FakeCoreError {
    fn from(value: serde_json::Error) -> Self {
        Self::InvalidJson(value)
    }
}

pub struct FakeCore {
    target_pid: u32,
    session_id: String,
    handshaken: bool,
    decoder: FrameDecoder,
}

impl FakeCore {
    pub fn new(target_pid: u32) -> Self {
        Self {
            target_pid,
            session_id: format!("fake-session-{target_pid}"),
            handshaken: false,
            decoder: FrameDecoder::default(),
        }
    }

    pub fn accept(&mut self, bytes: &[u8]) -> Result<Vec<Vec<u8>>, FakeCoreError> {
        let frames = self.decoder.push(bytes)?;
        frames
            .into_iter()
            .map(|frame| self.handle_frame(frame))
            .collect()
    }

    pub fn disconnect(&mut self) {
        self.handshaken = false;
        self.decoder = FrameDecoder::default();
    }

    fn handle_frame(&mut self, frame: Frame) -> Result<Vec<u8>, FakeCoreError> {
        if frame.header.minor != PROTOCOL_MINOR {
            return Err(FakeCoreError::InvalidEnvelope(
                "frame minor version is not supported",
            ));
        }
        match frame.header.kind {
            FrameKind::Hello => self.handle_hello(frame),
            FrameKind::Request => self.handle_request(frame),
            FrameKind::Ping if self.handshaken => self.handle_ping(frame),
            FrameKind::Cancel if self.handshaken => self.handle_cancel(frame),
            FrameKind::Shutdown if self.handshaken => self.handle_shutdown(frame),
            kind if !self.handshaken => Err(match kind {
                FrameKind::Request | FrameKind::Ping | FrameKind::Cancel | FrameKind::Shutdown => {
                    FakeCoreError::HandshakeRequired
                }
                _ => FakeCoreError::UnexpectedFrame(kind),
            }),
            kind => Err(FakeCoreError::UnexpectedFrame(kind)),
        }
    }

    fn handle_hello(&mut self, frame: Frame) -> Result<Vec<u8>, FakeCoreError> {
        if self.handshaken {
            return Err(FakeCoreError::DuplicateHandshake);
        }
        if frame.header.request_id == 0 {
            return Err(FakeCoreError::InvalidEnvelope(
                "Hello request ID must be positive",
            ));
        }
        let hello: HelloPayload = serde_json::from_slice(&frame.payload)?;
        if hello.protocol.major != PROTOCOL_MAJOR {
            return Err(FakeCoreError::Protocol(ProtocolError::UnsupportedMajor(
                hello.protocol.major,
            )));
        }
        if hello.protocol.minor != PROTOCOL_MINOR
            || hello.host_version.is_empty()
            || hello.host_version.len() > 64
        {
            return Err(FakeCoreError::InvalidEnvelope(
                "Hello version or Host identity is invalid",
            ));
        }
        if hello.target_pid != self.target_pid {
            return Err(FakeCoreError::TargetPidMismatch {
                expected: self.target_pid,
                actual: hello.target_pid,
            });
        }

        self.handshaken = true;
        let welcome = WelcomePayload {
            core_version: "fake-core-0.1.0".to_string(),
            protocol: ProtocolVersion {
                major: PROTOCOL_MAJOR,
                minor: PROTOCOL_MINOR,
            },
            session_id: self.session_id.clone(),
            target_pid: self.target_pid,
            engine_profile: None,
            capabilities: BTreeMap::from([
                ("engine.core".to_string(), true),
                ("status.inspect".to_string(), true),
                ("transport.named_pipe".to_string(), true),
            ]),
            limits: ProtocolLimits {
                max_payload_bytes: MAX_PAYLOAD_SIZE,
                pending_rpc_per_session: 256,
                game_thread_tasks: 128,
                hook_event_ring: 8_192,
                subscriber_events: 1_024,
                dump_running: 1,
                max_timeout_ms: 120_000,
            },
        };
        Ok(encode_frame(
            FrameKind::Welcome,
            frame.header.request_id,
            &serde_json::to_vec(&welcome)?,
        )?)
    }

    fn handle_request(&self, frame: Frame) -> Result<Vec<u8>, FakeCoreError> {
        if !self.handshaken {
            return Err(FakeCoreError::HandshakeRequired);
        }
        if frame.header.request_id == 0 {
            return Err(FakeCoreError::InvalidEnvelope(
                "Request request ID must be positive",
            ));
        }
        let request: RequestPayload = serde_json::from_slice(&frame.payload)?;
        if request.session_id != self.session_id {
            return Err(FakeCoreError::SessionMismatch);
        }

        let response = if request.timeout_ms == 0 {
            self.failure_response(
                frame.header.request_id,
                "DEADLINE_EXPIRED",
                "request deadline expired",
            )
        } else if request.operation == "status.inspect" {
            ResponsePayload {
                ok: true,
                request_id: frame.header.request_id,
                session_id: self.session_id.clone(),
                error: None,
                data: json!({ "state": "ready" }),
                timing: RpcTiming {
                    queued_us: 0,
                    execute_us: 1,
                },
            }
        } else {
            self.failure_response(
                frame.header.request_id,
                "OPERATION_UNSUPPORTED",
                "operation is not implemented by Fake Core",
            )
        };

        Ok(encode_frame(
            FrameKind::Response,
            frame.header.request_id,
            &serde_json::to_vec(&response)?,
        )?)
    }

    fn handle_ping(&self, frame: Frame) -> Result<Vec<u8>, FakeCoreError> {
        self.require_correlated(frame.header.request_id, "Ping")?;
        let heartbeat: HeartbeatPayload = serde_json::from_slice(&frame.payload)?;
        self.require_session(&heartbeat.session_id)?;
        if heartbeat.nonce == 0 || heartbeat.sent_at_monotonic_us == 0 {
            return Err(FakeCoreError::InvalidEnvelope(
                "Ping heartbeat values must be positive",
            ));
        }
        Ok(encode_frame(
            FrameKind::Pong,
            frame.header.request_id,
            &serde_json::to_vec(&heartbeat)?,
        )?)
    }

    fn handle_cancel(&self, frame: Frame) -> Result<Vec<u8>, FakeCoreError> {
        self.require_correlated(frame.header.request_id, "Cancel")?;
        let cancel: CancelPayload = serde_json::from_slice(&frame.payload)?;
        self.require_session(&cancel.session_id)?;
        if cancel.reason.is_empty() || cancel.reason.len() > 256 {
            return Err(FakeCoreError::InvalidEnvelope("Cancel reason is invalid"));
        }
        let response = self.failure_response(
            frame.header.request_id,
            "REQUEST_CANCELLED",
            "request was cancelled",
        );
        Ok(encode_frame(
            FrameKind::Response,
            frame.header.request_id,
            &serde_json::to_vec(&response)?,
        )?)
    }

    fn handle_shutdown(&mut self, frame: Frame) -> Result<Vec<u8>, FakeCoreError> {
        self.require_correlated(frame.header.request_id, "Shutdown")?;
        let shutdown: ShutdownPayload = serde_json::from_slice(&frame.payload)?;
        self.require_session(&shutdown.session_id)?;
        if shutdown.reason.is_empty() || shutdown.reason.len() > 256 {
            return Err(FakeCoreError::InvalidEnvelope("Shutdown reason is invalid"));
        }
        let acknowledgement = encode_frame(
            FrameKind::Shutdown,
            frame.header.request_id,
            &serde_json::to_vec(&shutdown)?,
        )?;
        self.handshaken = false;
        Ok(acknowledgement)
    }

    fn failure_response(&self, request_id: u64, code: &str, message: &str) -> ResponsePayload {
        ResponsePayload {
            ok: false,
            request_id,
            session_id: self.session_id.clone(),
            error: Some(RpcErrorPayload {
                code: code.to_string(),
                message: message.to_string(),
                details: json!({}),
            }),
            data: Value::Null,
            timing: RpcTiming {
                queued_us: 0,
                execute_us: 0,
            },
        }
    }

    fn require_session(&self, session_id: &str) -> Result<(), FakeCoreError> {
        if session_id != self.session_id {
            return Err(FakeCoreError::SessionMismatch);
        }
        Ok(())
    }

    fn require_correlated(&self, request_id: u64, kind: &'static str) -> Result<(), FakeCoreError> {
        if request_id == 0 {
            return Err(FakeCoreError::InvalidEnvelope(match kind {
                "Ping" => "Ping request ID must be positive",
                "Cancel" => "Cancel request ID must be positive",
                "Shutdown" => "Shutdown request ID must be positive",
                _ => "correlated request ID must be positive",
            }));
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use uexplorer_protocol::{FrameDecoder, FrameKind};

    fn hello(pid: u32) -> Vec<u8> {
        let payload = json!({
            "host_version": "test-host",
            "protocol": { "major": 1, "minor": 0 },
            "target_pid": pid
        });
        encode_frame(FrameKind::Hello, 1, &serde_json::to_vec(&payload).unwrap()).unwrap()
    }

    fn handshake(core: &mut FakeCore) {
        let output = core.accept(&hello(core.target_pid)).unwrap();
        assert_eq!(output.len(), 1);
        let frames = FrameDecoder::default().push(&output[0]).unwrap();
        assert_eq!(frames[0].header.kind, FrameKind::Welcome);
        assert_eq!(frames[0].header.request_id, 1);
        let welcome: WelcomePayload = serde_json::from_slice(&frames[0].payload).unwrap();
        assert_eq!(welcome.protocol.minor, PROTOCOL_MINOR);
        assert_eq!(welcome.limits.pending_rpc_per_session, 256);
        assert!(welcome.capabilities["transport.named_pipe"]);
    }

    #[test]
    fn partial_hello_produces_one_welcome() {
        let mut core = FakeCore::new(4242);
        let input = hello(4242);
        let mut output = Vec::new();
        for chunk in input.chunks(3) {
            output.extend(core.accept(chunk).unwrap());
        }
        assert_eq!(output.len(), 1);
        assert_eq!(FrameDecoder::default().push(&output[0]).unwrap().len(), 1);
    }

    #[test]
    fn target_pid_mismatch_is_not_recovered() {
        let mut core = FakeCore::new(4242);
        assert!(matches!(
            core.accept(&hello(1111)),
            Err(FakeCoreError::TargetPidMismatch {
                expected: 4242,
                actual: 1111
            })
        ));
    }

    #[test]
    fn deadline_is_explicit_failure() {
        let mut core = FakeCore::new(4242);
        handshake(&mut core);
        let request = json!({
            "operation": "status.inspect",
            "session_id": core.session_id,
            "timeout_ms": 0,
            "data": {}
        });
        let input = encode_frame(
            FrameKind::Request,
            2,
            &serde_json::to_vec(&request).unwrap(),
        )
        .unwrap();
        let output = core.accept(&input).unwrap();
        let response = FrameDecoder::default().push(&output[0]).unwrap();
        let payload: Value = serde_json::from_slice(&response[0].payload).unwrap();
        assert_eq!(payload["ok"], false);
        assert_eq!(payload["error"]["code"], "DEADLINE_EXPIRED");
    }

    #[test]
    fn ping_cancel_and_shutdown_use_typed_correlated_envelopes() {
        let mut core = FakeCore::new(4242);
        handshake(&mut core);

        let heartbeat = HeartbeatPayload {
            session_id: core.session_id.clone(),
            nonce: 7,
            sent_at_monotonic_us: 1_000,
        };
        let ping =
            encode_frame(FrameKind::Ping, 2, &serde_json::to_vec(&heartbeat).unwrap()).unwrap();
        let pong = core.accept(&ping).unwrap();
        let pong = FrameDecoder::default().push(&pong[0]).unwrap();
        assert_eq!(pong[0].header.kind, FrameKind::Pong);
        assert_eq!(pong[0].header.request_id, 2);

        let cancel = CancelPayload {
            session_id: core.session_id.clone(),
            reason: "caller_cancelled".to_string(),
        };
        let cancel =
            encode_frame(FrameKind::Cancel, 3, &serde_json::to_vec(&cancel).unwrap()).unwrap();
        let cancelled = core.accept(&cancel).unwrap();
        let cancelled = FrameDecoder::default().push(&cancelled[0]).unwrap();
        let response: ResponsePayload = serde_json::from_slice(&cancelled[0].payload).unwrap();
        assert_eq!(response.error.unwrap().code, "REQUEST_CANCELLED");

        let shutdown = ShutdownPayload {
            session_id: core.session_id.clone(),
            reason: "host_shutdown".to_string(),
        };
        let shutdown = encode_frame(
            FrameKind::Shutdown,
            4,
            &serde_json::to_vec(&shutdown).unwrap(),
        )
        .unwrap();
        let acknowledgement = core.accept(&shutdown).unwrap();
        let acknowledgement = FrameDecoder::default().push(&acknowledgement[0]).unwrap();
        assert_eq!(acknowledgement[0].header.kind, FrameKind::Shutdown);
        assert!(!core.handshaken);
    }

    #[test]
    fn disconnect_requires_a_new_handshake() {
        let mut core = FakeCore::new(4242);
        handshake(&mut core);
        core.disconnect();
        let input = encode_frame(FrameKind::Ping, 3, &[]).unwrap();
        assert!(matches!(
            core.accept(&input),
            Err(FakeCoreError::HandshakeRequired)
        ));
    }
}
