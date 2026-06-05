#pragma once

#include <cstdint>

#include "model/bytes.hpp"
#include "model/protocol_types.hpp"

namespace axtp {

struct Message {
    std::uint16_t messageId = 0;
    PayloadType payloadType = PayloadType::Rpc;
    Bytes body;
};

}  // namespace axtp
