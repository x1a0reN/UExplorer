#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace UExplorer::IPC
{
	inline constexpr std::array<std::uint8_t, 4> Magic{ 'U', 'E', 'X', 'P' };
	inline constexpr std::uint16_t ProtocolMajor = 1;
	inline constexpr std::uint16_t ProtocolMinor = 0;
	inline constexpr std::size_t HeaderSize = 24;
	inline constexpr std::uint32_t MaxPayloadSize = 8U * 1024U * 1024U;

	enum class FrameKind : std::uint16_t
	{
		Hello = 1,
		Welcome = 2,
		Request = 3,
		Response = 4,
		Event = 5,
		Cancel = 6,
		Ping = 7,
		Pong = 8,
		Shutdown = 9,
	};

	enum class ProtocolError
	{
		None,
		IncompleteHeader,
		InvalidMagic,
		UnsupportedMajor,
		UnknownKind,
		UnsupportedFlags,
		PayloadTooLarge,
		InvalidPayloadLimit,
		DecoderFailed,
	};

	struct FrameHeader
	{
		std::uint16_t Major = ProtocolMajor;
		std::uint16_t Minor = ProtocolMinor;
		FrameKind Kind = FrameKind::Request;
		std::uint16_t Flags = 0;
		std::uint32_t PayloadLength = 0;
		std::uint64_t RequestId = 0;
	};

	struct Frame
	{
		FrameHeader Header;
		std::vector<std::uint8_t> Payload;
	};

	struct DecodeBatch
	{
		ProtocolError Error = ProtocolError::None;
		std::vector<Frame> Frames;
	};

	namespace Detail
	{
		inline std::uint16_t ReadU16(const std::uint8_t* data)
		{
			return static_cast<std::uint16_t>(data[0])
				| (static_cast<std::uint16_t>(data[1]) << 8U);
		}

		inline std::uint32_t ReadU32(const std::uint8_t* data)
		{
			return static_cast<std::uint32_t>(data[0])
				| (static_cast<std::uint32_t>(data[1]) << 8U)
				| (static_cast<std::uint32_t>(data[2]) << 16U)
				| (static_cast<std::uint32_t>(data[3]) << 24U);
		}

		inline std::uint64_t ReadU64(const std::uint8_t* data)
		{
			std::uint64_t value = 0;
			for (std::size_t index = 0; index < 8; ++index)
			{
				value |= static_cast<std::uint64_t>(data[index]) << (index * 8U);
			}
			return value;
		}

		inline void WriteU16(std::uint8_t* output, const std::uint16_t value)
		{
			output[0] = static_cast<std::uint8_t>(value & 0xFFU);
			output[1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
		}

		inline void WriteU32(std::uint8_t* output, const std::uint32_t value)
		{
			for (std::size_t index = 0; index < 4; ++index)
			{
				output[index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
			}
		}

		inline void WriteU64(std::uint8_t* output, const std::uint64_t value)
		{
			for (std::size_t index = 0; index < 8; ++index)
			{
				output[index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
			}
		}

		inline bool IsKnownKind(const FrameKind kind)
		{
			switch (kind)
			{
			case FrameKind::Hello:
			case FrameKind::Welcome:
			case FrameKind::Request:
			case FrameKind::Response:
			case FrameKind::Event:
			case FrameKind::Cancel:
			case FrameKind::Ping:
			case FrameKind::Pong:
			case FrameKind::Shutdown:
				return true;
			default:
				return false;
			}
		}
	}

	inline ProtocolError DecodeHeader(const std::span<const std::uint8_t> bytes, FrameHeader& output)
	{
		if (bytes.size() < HeaderSize)
			return ProtocolError::IncompleteHeader;
		if (!std::equal(Magic.begin(), Magic.end(), bytes.begin()))
			return ProtocolError::InvalidMagic;

		output.Major = Detail::ReadU16(bytes.data() + 4);
		output.Minor = Detail::ReadU16(bytes.data() + 6);
		output.Kind = static_cast<FrameKind>(Detail::ReadU16(bytes.data() + 8));
		output.Flags = Detail::ReadU16(bytes.data() + 10);
		output.PayloadLength = Detail::ReadU32(bytes.data() + 12);
		output.RequestId = Detail::ReadU64(bytes.data() + 16);

		if (output.Major != ProtocolMajor)
			return ProtocolError::UnsupportedMajor;
		if (!Detail::IsKnownKind(output.Kind))
			return ProtocolError::UnknownKind;
		if (output.Flags != 0)
			return ProtocolError::UnsupportedFlags;
		if (output.PayloadLength > MaxPayloadSize)
			return ProtocolError::PayloadTooLarge;
		return ProtocolError::None;
	}

	inline ProtocolError EncodeFrame(
		const FrameKind kind,
		const std::uint64_t requestId,
		const std::span<const std::uint8_t> payload,
		std::vector<std::uint8_t>& output,
		const std::uint16_t minor = ProtocolMinor)
	{
		if (!Detail::IsKnownKind(kind))
			return ProtocolError::UnknownKind;
		if (payload.size() > MaxPayloadSize)
			return ProtocolError::PayloadTooLarge;

		output.assign(HeaderSize + payload.size(), 0);
		std::copy(Magic.begin(), Magic.end(), output.begin());
		Detail::WriteU16(output.data() + 4, ProtocolMajor);
		Detail::WriteU16(output.data() + 6, minor);
		Detail::WriteU16(output.data() + 8, static_cast<std::uint16_t>(kind));
		Detail::WriteU16(output.data() + 10, 0);
		Detail::WriteU32(output.data() + 12, static_cast<std::uint32_t>(payload.size()));
		Detail::WriteU64(output.data() + 16, requestId);
		std::copy(payload.begin(), payload.end(), output.begin() + HeaderSize);
		return ProtocolError::None;
	}

	class FrameDecoder
	{
	public:
		ProtocolError SetPayloadLimit(const std::uint32_t limit)
		{
			if (m_Error != ProtocolError::None)
				return ProtocolError::DecoderFailed;
			if (limit == 0 || limit > MaxPayloadSize)
				return ProtocolError::InvalidPayloadLimit;

			m_MaxPayloadSize = limit;
			if (m_Buffer.size() >= HeaderSize)
			{
				FrameHeader header;
				const ProtocolError error = DecodeHeader(
					std::span<const std::uint8_t>(m_Buffer).first(HeaderSize),
					header);
				if (error != ProtocolError::None || header.PayloadLength > m_MaxPayloadSize)
				{
					m_Error = error == ProtocolError::None
						? ProtocolError::PayloadTooLarge
						: error;
					m_Buffer.clear();
					return m_Error;
				}
			}
			return ProtocolError::None;
		}

		DecodeBatch Push(const std::span<const std::uint8_t> bytes)
		{
			DecodeBatch result;
			if (m_Error != ProtocolError::None)
			{
				result.Error = ProtocolError::DecoderFailed;
				return result;
			}

			std::span<const std::uint8_t> remaining = bytes;
			while (!remaining.empty())
			{
				if (m_Buffer.size() < HeaderSize)
				{
					const std::size_t take = (std::min)(HeaderSize - m_Buffer.size(), remaining.size());
					m_Buffer.insert(m_Buffer.end(), remaining.begin(), remaining.begin() + static_cast<std::ptrdiff_t>(take));
					remaining = remaining.subspan(take);
					if (m_Buffer.size() < HeaderSize)
						break;
				}

				FrameHeader header;
				const auto headerBytes = std::span<const std::uint8_t>(m_Buffer).first(HeaderSize);
				const auto error = DecodeHeader(headerBytes, header);
				if (error != ProtocolError::None || header.PayloadLength > m_MaxPayloadSize)
				{
					m_Error = error == ProtocolError::None
						? ProtocolError::PayloadTooLarge
						: error;
					m_Buffer.clear();
					result.Frames.clear();
					result.Error = m_Error;
					return result;
				}

				const auto frameSize = HeaderSize + static_cast<std::size_t>(header.PayloadLength);
				const std::size_t take = (std::min)(frameSize - m_Buffer.size(), remaining.size());
				m_Buffer.insert(m_Buffer.end(), remaining.begin(), remaining.begin() + static_cast<std::ptrdiff_t>(take));
				remaining = remaining.subspan(take);
				if (m_Buffer.size() < frameSize)
					break;

				Frame frame;
				frame.Header = header;
				const auto payloadStart = m_Buffer.begin() + static_cast<std::ptrdiff_t>(HeaderSize);
				const auto payloadEnd = payloadStart + static_cast<std::ptrdiff_t>(header.PayloadLength);
				frame.Payload.assign(payloadStart, payloadEnd);
				result.Frames.push_back(std::move(frame));
				m_Buffer.clear();
			}
			return result;
		}

		ProtocolError Error() const { return m_Error; }
		std::size_t BufferedBytes() const { return m_Buffer.size(); }

	private:
		std::vector<std::uint8_t> m_Buffer;
		ProtocolError m_Error = ProtocolError::None;
		std::uint32_t m_MaxPayloadSize = MaxPayloadSize;
	};
}
