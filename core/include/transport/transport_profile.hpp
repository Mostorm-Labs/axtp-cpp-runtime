#pragma once

#include <cstddef>

#include "model/payload.hpp"

namespace axtp {

enum class TransportKind {
    Tcp,
    WebSocket,
    Hid,
    Ble,
    Uart,
    Mock,
    Custom,
};

enum class AxtpWireMode {
    FramedBinary,
    WebSocketJsonRpc,
};

struct TransportProfile {
    TransportKind kind = TransportKind::Custom;
    AxtpWireMode wireMode = AxtpWireMode::FramedBinary;
    RpcEncoding defaultRpcEncoding = RpcEncoding::Tlv;
    bool messageOriented = false;
    bool supportsTextMessage = false;
    bool supportsBinaryMessage = true;
    std::size_t preferredFrameSize = 4096;
};

}  // namespace axtp
