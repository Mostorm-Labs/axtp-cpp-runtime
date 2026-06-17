#pragma once

#include <cstddef>
#include <cstdint>

#include "io/byte_reader.hpp"
#include "io/byte_writer.hpp"
#include "model/payload.hpp"

namespace axtp {

class ControlTlvCodec {
public:
    static constexpr std::uint8_t kSessionId = 0x01;
    static constexpr std::uint8_t kProtocolVersion = 0x02;
    static constexpr std::uint8_t kMaxFrameSize = 0x04;
    static constexpr std::uint8_t kMtu = 0x06;
    static constexpr std::uint8_t kSupportedPayloadTypes = 0x07;
    static constexpr std::uint8_t kSupportedRpcEncodings = 0x08;
    static constexpr std::uint8_t kHeartbeatIntervalMs = 0x0A;
    static constexpr std::uint8_t kAckMode = 0x0B;
    static constexpr std::uint8_t kSelectedRpcEncoding = 0x1E;

    static ControlTlvOptions defaultsForOpen() {
        ControlTlvOptions options;
        options.hasProtocolVersion = true;
        options.hasMaxFrameSize = true;
        options.hasMtu = true;
        options.hasSupportedPayloadTypes = true;
        options.hasSupportedRpcEncodings = true;
        options.hasHeartbeatIntervalMs = true;
        options.hasAckMode = true;
        options.protocolVersion = 1;
        options.maxFrameSize = 4096;
        options.mtu = 4096;
        options.supportedPayloadTypes = 0x07;
        options.supportedRpcEncodings = 0x09;
        options.heartbeatIntervalMs = 1000;
        options.ackMode = 0;
        return options;
    }

    static ControlTlvOptions defaultsForAccept(const ControlTlvOptions& requested) {
        ControlTlvOptions options;
        options.hasSessionId = true;
        options.hasProtocolVersion = true;
        options.hasMaxFrameSize = true;
        options.hasMtu = true;
        options.hasSupportedPayloadTypes = true;
        options.hasSelectedRpcEncoding = true;
        options.hasHeartbeatIntervalMs = true;
        options.hasAckMode = true;
        options.sessionId = requested.hasSessionId ? requested.sessionId : 0;
        options.protocolVersion = requested.hasProtocolVersion ? requested.protocolVersion : 1;
        options.maxFrameSize = requested.hasMaxFrameSize ? requested.maxFrameSize : 4096;
        options.mtu = requested.hasMtu ? requested.mtu : 4096;
        options.supportedPayloadTypes =
            requested.hasSupportedPayloadTypes ? requested.supportedPayloadTypes : 0x07;
        options.selectedRpcEncoding = static_cast<std::uint8_t>(RpcEncoding::Json);
        if (requested.hasSupportedRpcEncodings &&
            (requested.supportedRpcEncodings & 0x01U) == 0) {
            options.valid = false;
        }
        options.heartbeatIntervalMs =
            requested.hasHeartbeatIntervalMs ? requested.heartbeatIntervalMs : 1000;
        options.ackMode = requested.hasAckMode ? requested.ackMode : 0;
        return options;
    }

    static Bytes encode(const ControlTlvOptions& options, bool includeAcceptFields) {
        ByteWriter writer;
        if (includeAcceptFields && options.hasSessionId) {
            writeU32(writer, kSessionId, options.sessionId);
        }
        if (options.hasProtocolVersion) {
            writeU8(writer, kProtocolVersion, options.protocolVersion);
        }
        if (options.hasMaxFrameSize) {
            writeU16OrU32(writer, kMaxFrameSize, options.maxFrameSize);
        }
        if (options.hasMtu) {
            writeU16OrU32(writer, kMtu, options.mtu);
        }
        if (options.hasSupportedPayloadTypes) {
            writeU8(writer, kSupportedPayloadTypes, options.supportedPayloadTypes);
        }
        if (!includeAcceptFields && options.hasSupportedRpcEncodings) {
            writeU8(writer, kSupportedRpcEncodings, options.supportedRpcEncodings);
        }
        if (includeAcceptFields && options.hasSelectedRpcEncoding) {
            writeU8(writer, kSelectedRpcEncoding, options.selectedRpcEncoding);
        }
        if (options.hasHeartbeatIntervalMs) {
            writeU16OrU32(writer, kHeartbeatIntervalMs, options.heartbeatIntervalMs);
        }
        if (options.hasAckMode) {
            writeU8(writer, kAckMode, options.ackMode);
        }
        return writer.takeBytes();
    }

    static ControlTlvOptions decode(const Bytes& bytes) {
        ControlTlvOptions options;
        if (bytes.empty()) {
            return options;
        }

        ByteReader reader(bytes);
        while (!reader.empty()) {
            const auto tag = reader.readU8();
            const auto length = reader.readU8();
            if (!tag.ok() || !length.ok() || !reader.hasRemaining(length.value)) {
                options.valid = false;
                return options;
            }
            const auto value = reader.readBytes(length.value);
            if (!value.ok()) {
                options.valid = false;
                return options;
            }
            decodeField(options, tag.value, value.value);
        }
        return options;
    }

private:
    static void writeHeader(ByteWriter& writer, std::uint8_t tag, std::uint8_t length) {
        writer.writeU8(tag);
        writer.writeU8(length);
    }

    static void writeU8(ByteWriter& writer, std::uint8_t tag, std::uint8_t value) {
        writeHeader(writer, tag, 1);
        writer.writeU8(value);
    }

    static void writeU16(ByteWriter& writer, std::uint8_t tag, std::uint16_t value) {
        writeHeader(writer, tag, 2);
        writer.writeU16(value);
    }

    static void writeU32(ByteWriter& writer, std::uint8_t tag, std::uint32_t value) {
        writeHeader(writer, tag, 4);
        writer.writeU32(value);
    }

    static void writeU16OrU32(ByteWriter& writer, std::uint8_t tag, std::uint32_t value) {
        if (value <= 0xFFFFU) {
            writeU16(writer, tag, static_cast<std::uint16_t>(value));
            return;
        }
        writeU32(writer, tag, value);
    }

    static bool readUnsigned(const Bytes& bytes, std::uint32_t maxValue, std::uint32_t* value) {
        if (bytes.empty() || bytes.size() > 4 || value == nullptr) {
            return false;
        }
        if (bytes.size() == 3) {
            return false;
        }

        std::uint32_t parsed = 0;
        for (const auto byte : bytes) {
            parsed = (parsed << 8U) | static_cast<std::uint32_t>(byte);
        }
        if (parsed > maxValue) {
            return false;
        }
        *value = parsed;
        return true;
    }

    static bool readU8(const Bytes& bytes, std::uint8_t* value) {
        std::uint32_t parsed = 0;
        if (!readUnsigned(bytes, 0xFFU, &parsed)) {
            return false;
        }
        *value = static_cast<std::uint8_t>(parsed);
        return true;
    }

    static bool readU16(const Bytes& bytes, std::uint16_t* value) {
        std::uint32_t parsed = 0;
        if (!readUnsigned(bytes, 0xFFFFU, &parsed)) {
            return false;
        }
        *value = static_cast<std::uint16_t>(parsed);
        return true;
    }

    static bool readU32(const Bytes& bytes, std::uint32_t* value) {
        return readUnsigned(bytes, 0xFFFFFFFFU, value);
    }

    static bool readU32Limited(const Bytes& bytes,
                               std::uint32_t maxValue,
                               std::uint32_t* value) {
        return readUnsigned(bytes, maxValue, value);
    }

    static bool readHeartbeat(const Bytes& bytes, std::uint32_t* value) {
        if (!readU32(bytes, value)) {
            return false;
        }
        return true;
    }

    static void decodeField(ControlTlvOptions& options, std::uint8_t tag, const Bytes& value) {
        switch (tag) {
        case kSessionId:
            options.hasSessionId = readU32(value, &options.sessionId);
            options.valid = options.valid && options.hasSessionId;
            break;
        case kProtocolVersion:
            options.hasProtocolVersion = readU8(value, &options.protocolVersion);
            options.valid = options.valid && options.hasProtocolVersion;
            break;
        case kMaxFrameSize:
            options.hasMaxFrameSize = readU32Limited(value, 0xFFFFU, &options.maxFrameSize);
            options.valid = options.valid && options.hasMaxFrameSize;
            break;
        case kMtu:
            options.hasMtu = readU32Limited(value, 0xFFFFU, &options.mtu);
            options.valid = options.valid && options.hasMtu;
            break;
        case kSupportedPayloadTypes:
            options.hasSupportedPayloadTypes =
                readU8(value, &options.supportedPayloadTypes);
            options.valid = options.valid && options.hasSupportedPayloadTypes;
            break;
        case kSupportedRpcEncodings:
            options.hasSupportedRpcEncodings =
                readU8(value, &options.supportedRpcEncodings);
            options.valid = options.valid && options.hasSupportedRpcEncodings;
            break;
        case kSelectedRpcEncoding:
            options.hasSelectedRpcEncoding = readU8(value, &options.selectedRpcEncoding);
            options.valid = options.valid && options.hasSelectedRpcEncoding;
            break;
        case kHeartbeatIntervalMs:
            options.hasHeartbeatIntervalMs = readHeartbeat(value, &options.heartbeatIntervalMs);
            options.valid = options.valid && options.hasHeartbeatIntervalMs;
            break;
        case kAckMode:
            options.hasAckMode = readU8(value, &options.ackMode);
            options.valid = options.valid && options.hasAckMode;
            break;
        default:
            break;
        }
    }
};

}  // namespace axtp
