#pragma once

#include <cstdint>

#include "core/protocol/model/bytes.hpp"
#include "core/protocol/model/protocol_types.hpp"

namespace axtp {

struct Message {
    std::uint16_t messageId = 0;
    PayloadType payloadType = PayloadType::Rpc;
    Bytes body;
};

}  // namespace axtp
