use std::fmt;

pub const MAGIC: [u8; 4] = *b"UEXP";
pub const PROTOCOL_MAJOR: u16 = 1;
pub const PROTOCOL_MINOR: u16 = 0;
pub const HEADER_SIZE: usize = 24;
pub const MAX_PAYLOAD_SIZE: u32 = 8 * 1024 * 1024;

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

#[derive(Default)]
pub struct FrameDecoder {
    buffer: Vec<u8>,
    failed: bool,
}

impl FrameDecoder {
    pub fn push(&mut self, bytes: &[u8]) -> Result<Vec<Frame>, ProtocolError> {
        if self.failed {
            return Err(ProtocolError::DecoderFailed);
        }

        self.buffer.extend_from_slice(bytes);
        let mut frames = Vec::new();
        let mut consumed = 0usize;

        while self.buffer.len() - consumed >= HEADER_SIZE {
            let header = match decode_header(&self.buffer[consumed..consumed + HEADER_SIZE]) {
                Ok(header) => header,
                Err(error) => {
                    self.failed = true;
                    self.buffer.clear();
                    return Err(error);
                }
            };
            let frame_size = HEADER_SIZE + header.payload_len as usize;
            if self.buffer.len() - consumed < frame_size {
                break;
            }

            let payload_start = consumed + HEADER_SIZE;
            let payload_end = consumed + frame_size;
            frames.push(Frame {
                header,
                payload: self.buffer[payload_start..payload_end].to_vec(),
            });
            consumed += frame_size;
        }

        if consumed > 0 {
            self.buffer.drain(..consumed);
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
        assert_eq!(
            FrameDecoder::default().push(&frame),
            Err(ProtocolError::PayloadTooLarge(MAX_PAYLOAD_SIZE + 1))
        );
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
}
