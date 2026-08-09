use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;
use std::fmt;

pub const MAGIC: [u8; 4] = *b"UEXP";
pub const PROTOCOL_MAJOR: u16 = 1;
pub const PROTOCOL_MINOR: u16 = 0;
pub const HEADER_SIZE: usize = 24;
pub const MAX_PAYLOAD_SIZE: u32 = 8 * 1024 * 1024;
pub const MAX_GENERATION: u64 = 9_007_199_254_740_991;
pub const MAX_SNAPSHOT_SOURCE_OBJECTS: u32 = 8_000_000;
pub const MAX_SNAPSHOT_PAGE_RECORDS: usize = 128;
pub const MAX_SNAPSHOT_NAME_BYTES: usize = 1_024;
pub const MAX_SNAPSHOT_PATH_BYTES: usize = 4_096;

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ObjectHandle {
    pub session_id: String,
    pub context_generation: u64,
    pub index: i32,
    pub serial: i32,
    pub address: String,
    pub class_fingerprint: String,
}

#[derive(Clone, Copy, Debug, Deserialize, Eq, Hash, PartialEq, Serialize)]
#[repr(u8)]
#[serde(rename_all = "lowercase")]
pub enum SnapshotObjectKind {
    Object,
    Package,
    Class,
    Struct,
    Enum,
    Function,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct SnapshotRecord {
    pub handle: ObjectHandle,
    pub name: String,
    pub full_path: String,
    pub class_path: String,
    pub package_path: String,
    pub kind: SnapshotObjectKind,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct SnapshotCursor {
    pub generation: u64,
    pub after_index: i32,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct SnapshotPage {
    pub generation: u64,
    pub context_generation: u64,
    pub captured_at_monotonic_us: u64,
    pub capture_duration_us: u64,
    pub source_object_count: u32,
    pub record_count: u32,
    pub skipped_slots: u32,
    pub items: Vec<SnapshotRecord>,
    pub has_more: bool,
    pub next_cursor: Option<SnapshotCursor>,
}

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ProtocolVersion {
    pub major: u16,
    pub minor: u16,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct HelloPayload {
    pub host_version: String,
    pub protocol: ProtocolVersion,
    pub target_pid: u32,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ProtocolLimits {
    pub max_payload_bytes: u32,
    pub pending_rpc_per_session: u32,
    pub game_thread_tasks: u32,
    pub hook_event_ring: u32,
    pub subscriber_events: u32,
    pub dump_running: u32,
    pub max_timeout_ms: u32,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct WelcomePayload {
    pub core_version: String,
    pub protocol: ProtocolVersion,
    pub session_id: String,
    pub target_pid: u32,
    pub engine_profile: Option<String>,
    pub capabilities: BTreeMap<String, bool>,
    pub limits: ProtocolLimits,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct RequestPayload {
    pub operation: String,
    pub session_id: String,
    pub timeout_ms: u32,
    pub data: Value,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct RpcErrorPayload {
    pub code: String,
    pub message: String,
    pub details: Value,
}

#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct RpcTiming {
    pub queued_us: u64,
    pub execute_us: u64,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ResponsePayload {
    pub ok: bool,
    pub request_id: u64,
    pub session_id: String,
    pub error: Option<RpcErrorPayload>,
    pub data: Value,
    pub timing: RpcTiming,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct EventPayload {
    pub seq: u64,
    pub kind: String,
    pub timestamp_us: u64,
    pub session_id: String,
    pub dropped_before: u64,
    pub data: Value,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct CancelPayload {
    pub session_id: String,
    pub reason: String,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct HeartbeatPayload {
    pub session_id: String,
    pub nonce: u64,
    pub sent_at_monotonic_us: u64,
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ShutdownPayload {
    pub session_id: String,
    pub reason: String,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u16)]
pub enum FrameKind {
    Hello = 1,
    Welcome = 2,
    Request = 3,
    Response = 4,
    Event = 5,
    Cancel = 6,
    Ping = 7,
    Pong = 8,
    Shutdown = 9,
}

impl TryFrom<u16> for FrameKind {
    type Error = ProtocolError;

    fn try_from(value: u16) -> Result<Self, Self::Error> {
        match value {
            1 => Ok(Self::Hello),
            2 => Ok(Self::Welcome),
            3 => Ok(Self::Request),
            4 => Ok(Self::Response),
            5 => Ok(Self::Event),
            6 => Ok(Self::Cancel),
            7 => Ok(Self::Ping),
            8 => Ok(Self::Pong),
            9 => Ok(Self::Shutdown),
            _ => Err(ProtocolError::UnknownKind(value)),
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ProtocolError {
    IncompleteHeader,
    InvalidMagic,
    UnsupportedMajor(u16),
    UnknownKind(u16),
    UnsupportedFlags(u16),
    PayloadTooLarge(u32),
    InvalidPayloadLimit(u32),
    DecoderFailed,
}

impl fmt::Display for ProtocolError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::IncompleteHeader => write!(formatter, "incomplete frame header"),
            Self::InvalidMagic => write!(formatter, "invalid frame magic"),
            Self::UnsupportedMajor(value) => {
                write!(formatter, "unsupported protocol major {value}")
            }
            Self::UnknownKind(value) => write!(formatter, "unknown frame kind {value}"),
            Self::UnsupportedFlags(value) => {
                write!(formatter, "unsupported frame flags {value:#x}")
            }
            Self::PayloadTooLarge(value) => {
                write!(formatter, "payload length {value} exceeds the v1 limit")
            }
            Self::InvalidPayloadLimit(value) => {
                write!(
                    formatter,
                    "decoder payload limit {value} is outside v1 bounds"
                )
            }
            Self::DecoderFailed => write!(formatter, "decoder is in a terminal failed state"),
        }
    }
}

impl std::error::Error for ProtocolError {}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct FrameHeader {
    pub major: u16,
    pub minor: u16,
    pub kind: FrameKind,
    pub flags: u16,
    pub payload_len: u32,
    pub request_id: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Frame {
    pub header: FrameHeader,
    pub payload: Vec<u8>,
}

pub fn decode_header(bytes: &[u8]) -> Result<FrameHeader, ProtocolError> {
    if bytes.len() < HEADER_SIZE {
        return Err(ProtocolError::IncompleteHeader);
    }
    if bytes[..4] != MAGIC {
        return Err(ProtocolError::InvalidMagic);
    }

    let major = u16::from_le_bytes([bytes[4], bytes[5]]);
    let minor = u16::from_le_bytes([bytes[6], bytes[7]]);
    let raw_kind = u16::from_le_bytes([bytes[8], bytes[9]]);
    let flags = u16::from_le_bytes([bytes[10], bytes[11]]);
    let payload_len = u32::from_le_bytes([bytes[12], bytes[13], bytes[14], bytes[15]]);
    let request_id = u64::from_le_bytes(bytes[16..24].try_into().expect("fixed header slice"));

    if major != PROTOCOL_MAJOR {
        return Err(ProtocolError::UnsupportedMajor(major));
    }
    let kind = FrameKind::try_from(raw_kind)?;
    if flags != 0 {
        return Err(ProtocolError::UnsupportedFlags(flags));
    }
    if payload_len > MAX_PAYLOAD_SIZE {
        return Err(ProtocolError::PayloadTooLarge(payload_len));
    }

    Ok(FrameHeader {
        major,
        minor,
        kind,
        flags,
        payload_len,
        request_id,
    })
}

pub fn encode_frame(
    kind: FrameKind,
    request_id: u64,
    payload: &[u8],
) -> Result<Vec<u8>, ProtocolError> {
    if payload.len() > MAX_PAYLOAD_SIZE as usize {
        return Err(ProtocolError::PayloadTooLarge(
            payload.len().try_into().unwrap_or(u32::MAX),
        ));
    }

    let mut output = Vec::with_capacity(HEADER_SIZE + payload.len());
    output.extend_from_slice(&MAGIC);
    output.extend_from_slice(&PROTOCOL_MAJOR.to_le_bytes());
    output.extend_from_slice(&PROTOCOL_MINOR.to_le_bytes());
    output.extend_from_slice(&(kind as u16).to_le_bytes());
    output.extend_from_slice(&0u16.to_le_bytes());
    output.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    output.extend_from_slice(&request_id.to_le_bytes());
    output.extend_from_slice(payload);
    Ok(output)
}

pub struct FrameDecoder {
    buffer: Vec<u8>,
    failed: bool,
    max_payload_size: u32,
}

impl Default for FrameDecoder {
    fn default() -> Self {
        Self {
            buffer: Vec::new(),
            failed: false,
            max_payload_size: MAX_PAYLOAD_SIZE,
        }
    }
}

impl FrameDecoder {
    pub fn set_payload_limit(&mut self, limit: u32) -> Result<(), ProtocolError> {
        if self.failed {
            return Err(ProtocolError::DecoderFailed);
        }
        if limit == 0 || limit > MAX_PAYLOAD_SIZE {
            return Err(ProtocolError::InvalidPayloadLimit(limit));
        }
        self.max_payload_size = limit;
        if self.buffer.len() >= HEADER_SIZE {
            let header = decode_header(&self.buffer[..HEADER_SIZE])?;
            if header.payload_len > limit {
                self.failed = true;
                self.buffer.clear();
                return Err(ProtocolError::PayloadTooLarge(header.payload_len));
            }
        }
        Ok(())
    }

    pub fn push(&mut self, bytes: &[u8]) -> Result<Vec<Frame>, ProtocolError> {
        if self.failed {
            return Err(ProtocolError::DecoderFailed);
        }

        let mut remaining = bytes;
        let mut frames = Vec::new();

        // Consume at most one frame into the internal buffer at a time. A peer can
        // coalesce arbitrary frames in one pipe read without making the decoder
        // duplicate the entire read or reserve from an untrusted payload length.
        while !remaining.is_empty() {
            if self.buffer.len() < HEADER_SIZE {
                let needed = HEADER_SIZE - self.buffer.len();
                let take = needed.min(remaining.len());
                self.buffer.extend_from_slice(&remaining[..take]);
                remaining = &remaining[take..];
                if self.buffer.len() < HEADER_SIZE {
                    break;
                }
            }

            let header = match decode_header(&self.buffer[..HEADER_SIZE]) {
                Ok(header) => header,
                Err(error) => {
                    self.failed = true;
                    self.buffer.clear();
                    return Err(error);
                }
            };
            if header.payload_len > self.max_payload_size {
                self.failed = true;
                self.buffer.clear();
                return Err(ProtocolError::PayloadTooLarge(header.payload_len));
            }
            let frame_size = HEADER_SIZE + header.payload_len as usize;
            let needed = frame_size - self.buffer.len();
            let take = needed.min(remaining.len());
            self.buffer.extend_from_slice(&remaining[..take]);
            remaining = &remaining[take..];
            if self.buffer.len() < frame_size {
                break;
            }

            frames.push(Frame {
                header,
                payload: self.buffer[HEADER_SIZE..frame_size].to_vec(),
            });
            self.buffer.clear();
        }
        Ok(frames)
    }

    pub fn buffered_bytes(&self) -> usize {
        self.buffer.len()
    }

    pub fn is_failed(&self) -> bool {
        self.failed
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn decode_hex(input: &str) -> Vec<u8> {
        let compact: String = input
            .chars()
            .filter(|value| !value.is_whitespace())
            .collect();
        assert_eq!(compact.len() % 2, 0);
        (0..compact.len())
            .step_by(2)
            .map(|index| u8::from_str_radix(&compact[index..index + 2], 16).unwrap())
            .collect()
    }

    #[test]
    fn golden_hello_decodes_one_byte_at_a_time() {
        let encoded = decode_hex(include_str!("../../v1/fixtures/hello.frame.hex"));
        let expected_payload = include_str!("../../v1/fixtures/hello.json").trim_end();
        let mut decoder = FrameDecoder::default();
        let mut frames = Vec::new();

        for byte in encoded {
            frames.extend(decoder.push(&[byte]).unwrap());
        }

        assert_eq!(frames.len(), 1);
        assert_eq!(frames[0].header.kind, FrameKind::Hello);
        assert_eq!(frames[0].header.request_id, 1);
        assert_eq!(frames[0].payload, expected_payload.as_bytes());
        assert_eq!(decoder.buffered_bytes(), 0);
    }

    #[test]
    fn coalesced_frames_are_all_returned() {
        let mut stream = encode_frame(FrameKind::Ping, 7, b"ok").unwrap();
        stream.extend_from_slice(&encode_frame(FrameKind::Pong, 7, &[]).unwrap());
        let frames = FrameDecoder::default().push(&stream).unwrap();
        assert_eq!(frames.len(), 2);
        assert_eq!(frames[0].header.kind, FrameKind::Ping);
        assert_eq!(frames[1].header.kind, FrameKind::Pong);
    }

    #[test]
    fn major_mismatch_is_terminal() {
        let mut frame = encode_frame(FrameKind::Request, 1, &[]).unwrap();
        frame[4..6].copy_from_slice(&2u16.to_le_bytes());
        let mut decoder = FrameDecoder::default();
        assert_eq!(
            decoder.push(&frame),
            Err(ProtocolError::UnsupportedMajor(2))
        );
        assert_eq!(decoder.push(&[]), Err(ProtocolError::DecoderFailed));
    }

    #[test]
    fn oversized_payload_is_rejected_before_allocation() {
        let mut frame = encode_frame(FrameKind::Request, 1, &[]).unwrap();
        frame[12..16].copy_from_slice(&(MAX_PAYLOAD_SIZE + 1).to_le_bytes());
        let mut decoder = FrameDecoder::default();
        assert_eq!(
            decoder.push(&frame),
            Err(ProtocolError::PayloadTooLarge(MAX_PAYLOAD_SIZE + 1))
        );
        assert_eq!(decoder.buffered_bytes(), 0);
        assert!(decoder.is_failed());
    }

    #[test]
    fn large_coalesced_input_keeps_only_one_partial_frame_buffered() {
        let frame = encode_frame(FrameKind::Ping, 7, b"{}").unwrap();
        let mut stream = Vec::with_capacity(frame.len() * 10_000 + HEADER_SIZE);
        for _ in 0..10_000 {
            stream.extend_from_slice(&frame);
        }
        stream.extend_from_slice(&encode_frame(FrameKind::Pong, 8, b"partial").unwrap()[..10]);

        let mut decoder = FrameDecoder::default();
        let frames = decoder.push(&stream).unwrap();
        assert_eq!(frames.len(), 10_000);
        assert_eq!(decoder.buffered_bytes(), 10);
    }

    #[test]
    fn negotiated_decoder_limit_rejects_the_header_before_buffering_its_body() {
        let mut decoder = FrameDecoder::default();
        decoder.set_payload_limit(64).unwrap();
        let oversized = encode_frame(FrameKind::Event, 0, &[0; 65]).unwrap();
        assert_eq!(
            decoder.push(&oversized),
            Err(ProtocolError::PayloadTooLarge(65))
        );
        assert_eq!(decoder.buffered_bytes(), 0);
        assert!(decoder.is_failed());
    }

    #[test]
    fn independent_usmap_consumer_accepts_uncompressed_golden_container() {
        let bytes = decode_hex(include_str!("../../v1/fixtures/usmap-none.hex"));
        assert_eq!(&bytes[..2], &[0xC4, 0x30]);
        assert_eq!(bytes[2], 4);
        assert_eq!(u32::from_le_bytes(bytes[3..7].try_into().unwrap()), 0);
        assert_eq!(bytes[7], 0);
        let compressed_size = u32::from_le_bytes(bytes[8..12].try_into().unwrap()) as usize;
        let uncompressed_size = u32::from_le_bytes(bytes[12..16].try_into().unwrap()) as usize;
        assert_eq!(compressed_size, 12);
        assert_eq!(uncompressed_size, compressed_size);
        assert_eq!(bytes.len(), 16 + compressed_size);
        assert_eq!(u32::from_le_bytes(bytes[16..20].try_into().unwrap()), 0);
        assert_eq!(u32::from_le_bytes(bytes[20..24].try_into().unwrap()), 0);
        assert_eq!(u32::from_le_bytes(bytes[24..28].try_into().unwrap()), 0);
    }

    #[test]
    fn snapshot_page_golden_payload_has_strict_typed_identity() {
        #[derive(Deserialize)]
        struct Envelope {
            data: SnapshotPage,
        }

        let envelope: Envelope = serde_json::from_str(include_str!(
            "../../v1/fixtures/object-snapshot-page-response.json"
        ))
        .unwrap();
        assert_eq!(envelope.data.generation, 9);
        assert_eq!(envelope.data.items.len(), 2);
        assert_eq!(envelope.data.items[1].kind, SnapshotObjectKind::Class);
        assert_eq!(
            envelope.data.next_cursor,
            Some(SnapshotCursor {
                generation: 9,
                after_index: 1
            })
        );

        let unknown_field = r#"{
            "generation":9,"after_index":1,"unexpected":true
        }"#;
        assert!(serde_json::from_str::<SnapshotCursor>(unknown_field).is_err());
    }

    #[test]
    fn rpc_golden_payloads_use_the_shared_strict_types() {
        let hello: HelloPayload =
            serde_json::from_str(include_str!("../../v1/fixtures/hello.json")).unwrap();
        assert_eq!(hello.protocol, ProtocolVersion { major: 1, minor: 0 });

        let welcome: WelcomePayload =
            serde_json::from_str(include_str!("../../v1/fixtures/welcome.json")).unwrap();
        assert_eq!(welcome.target_pid, 4242);
        assert_eq!(welcome.limits.pending_rpc_per_session, 256);
        assert!(welcome.capabilities["transport.named_pipe"]);

        let request: RequestPayload = serde_json::from_str(include_str!(
            "../../v1/fixtures/object-snapshot-page-request.json"
        ))
        .unwrap();
        assert_eq!(request.operation, "objects.snapshot.page");

        let response: ResponsePayload = serde_json::from_str(include_str!(
            "../../v1/fixtures/object-handle-response.json"
        ))
        .unwrap();
        assert!(response.ok);
        assert_eq!(response.request_id, 42);

        let heartbeat: HeartbeatPayload =
            serde_json::from_str(include_str!("../../v1/fixtures/heartbeat.json")).unwrap();
        assert_eq!(heartbeat.nonce, 7);

        let shutdown: ShutdownPayload =
            serde_json::from_str(include_str!("../../v1/fixtures/shutdown.json")).unwrap();
        assert_eq!(shutdown.reason, "host_exit");

        let unknown_welcome = serde_json::json!({
            "core_version": welcome.core_version,
            "protocol": welcome.protocol,
            "session_id": welcome.session_id,
            "target_pid": welcome.target_pid,
            "engine_profile": welcome.engine_profile,
            "capabilities": welcome.capabilities,
            "limits": welcome.limits,
            "legacy_token": "forbidden"
        });
        assert!(serde_json::from_value::<WelcomePayload>(unknown_welcome).is_err());
    }
}
