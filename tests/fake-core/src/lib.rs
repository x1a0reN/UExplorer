use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::fmt;
use uexplorer_protocol::{encode_frame, Frame, FrameDecoder, FrameKind, ProtocolError};

#[derive(Debug)]
pub enum FakeCoreError {
    Protocol(ProtocolError),
    InvalidJson(serde_json::Error),
    UnexpectedFrame(FrameKind),
    HandshakeRequired,
    DuplicateHandshake,
    TargetPidMismatch { expected: u32, actual: u32 },
    SessionMismatch,
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

#[derive(Deserialize)]
struct ProtocolVersion {
    major: u16,
    #[serde(rename = "minor")]
    _minor: u16,
}

#[derive(Deserialize)]
struct Hello {
    #[serde(rename = "host_version")]
    _host_version: String,
    protocol: ProtocolVersion,
    target_pid: u32,
}

#[derive(Serialize)]
struct Welcome<'a> {
    core_version: &'a str,
    protocol: VersionPayload,
    session_id: &'a str,
    target_pid: u32,
    engine_profile: Option<&'a str>,
    capabilities: Value,
    limits: Value,
}

#[derive(Serialize)]
struct VersionPayload {
    major: u16,
    minor: u16,
}

#[derive(Deserialize)]
struct Request {
    operation: String,
    session_id: String,
    timeout_ms: u32,
    #[serde(rename = "data")]
    _data: Value,
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
        match frame.header.kind {
            FrameKind::Hello => self.handle_hello(frame),
            FrameKind::Request => self.handle_request(frame),
            FrameKind::Ping if self.handshaken => Ok(encode_frame(
                FrameKind::Pong,
                frame.header.request_id,
                &frame.payload,
            )?),
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
        let hello: Hello = serde_json::from_slice(&frame.payload)?;
        if hello.protocol.major != 1 {
            return Err(FakeCoreError::Protocol(ProtocolError::UnsupportedMajor(
                hello.protocol.major,
            )));
        }
        if hello.target_pid != self.target_pid {
            return Err(FakeCoreError::TargetPidMismatch {
                expected: self.target_pid,
                actual: hello.target_pid,
            });
        }

        self.handshaken = true;
        let welcome = Welcome {
            core_version: "fake-core-0.1.0",
            protocol: VersionPayload { major: 1, minor: 0 },
            session_id: &self.session_id,
            target_pid: self.target_pid,
            engine_profile: None,
            capabilities: json!({ "status": true }),
            limits: json!({ "max_payload_bytes": 8_388_608, "pending_rpc": 256 }),
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
        let request: Request = serde_json::from_slice(&frame.payload)?;
        if request.session_id != self.session_id {
            return Err(FakeCoreError::SessionMismatch);
        }

        let response = if request.timeout_ms == 0 {
            json!({
                "ok": false,
                "request_id": frame.header.request_id,
                "session_id": self.session_id,
                "error": { "code": "DEADLINE_EXPIRED", "message": "request deadline expired", "details": {} },
                "data": null,
                "timing": { "queued_us": 0, "execute_us": 0 }
            })
        } else if request.operation == "status.get" {
            json!({
                "ok": true,
                "request_id": frame.header.request_id,
                "session_id": self.session_id,
                "error": null,
                "data": { "state": "ready" },
                "timing": { "queued_us": 0, "execute_us": 1 }
            })
        } else {
            json!({
                "ok": false,
                "request_id": frame.header.request_id,
                "session_id": self.session_id,
                "error": { "code": "OPERATION_UNSUPPORTED", "message": "operation is not implemented by Fake Core", "details": {} },
                "data": null,
                "timing": { "queued_us": 0, "execute_us": 0 }
            })
        };

        Ok(encode_frame(
            FrameKind::Response,
            frame.header.request_id,
            &serde_json::to_vec(&response)?,
        )?)
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
            "operation": "status.get",
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
