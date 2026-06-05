#pragma once

#include <cstddef>

#include "io/byte_sink.hpp"
#include "model/bytes.hpp"

namespace axtp {

template <typename Endpoint>
class EndpointByteSink final : public IByteSink {
public:
    explicit EndpointByteSink(Endpoint& endpoint)
        : _endpoint(endpoint) {}

    void onBytes(const Byte* data, std::size_t size) override {
        _endpoint.onTransportBytes(data, size);
    }

private:
    Endpoint& _endpoint;
};

}  // namespace axtp
