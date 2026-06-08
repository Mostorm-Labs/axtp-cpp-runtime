#pragma once

#include <cstdint>

#include "generated/axtp_ids_generated.h"

namespace axtp {

inline constexpr std::uint8_t kAxtpStandardMagic0 = 0x41;
inline constexpr std::uint8_t kAxtpStandardMagic1 = 0x58;
inline constexpr std::uint8_t kAxtpVersion1 = 0x01;
inline constexpr std::uint8_t kStandardFrameHeaderSize = 12;
inline constexpr std::uint8_t kStandardFrameCrcSize = 2;
inline constexpr std::uint8_t kControlPayloadHeaderSize = 5;
inline constexpr std::uint8_t kBinaryRpcHeaderSize = 11;
inline constexpr std::uint8_t kStreamPayloadHeaderSize = 16;
inline constexpr std::uint8_t kRpcEncodingJsonBinaryValue = 0x04;

enum class SourceProtocol : std::uint8_t {
    AxtpV1 = 0x01,
    JsonRpc = 0x02,
};

inline constexpr RpcEncoding jsonBinaryRpcEncoding() {
    return static_cast<RpcEncoding>(kRpcEncodingJsonBinaryValue);
}

inline constexpr bool isJsonBinaryRpcEncoding(RpcEncoding encoding) {
    return static_cast<std::uint8_t>(encoding) == kRpcEncodingJsonBinaryValue;
}

inline constexpr RpcBodyEncoding bodyEncodingForRpcEncoding(RpcEncoding encoding) {
    return isJsonBinaryRpcEncoding(encoding) ? RpcBodyEncoding::Tlv8 : RpcBodyEncoding::None;
}

}  // namespace axtp
