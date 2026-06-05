#pragma once

#include <optional>
#include <queue>
#include <utility>

#include "transport/transport.hpp"

namespace axtp {

class MockTransport : public ITransport {
public:
    void bind(IByteSink& sink) override {
        _sink = &sink;
    }

    void open() override {
        _open = true;
    }

    void close() override {
        _open = false;
    }

    void injectIncoming(const Bytes& bytes) {
        if (_sink != nullptr) {
            _sink->onBytes(bytes.data(), bytes.size());
        }
    }

    void sendBytes(const Byte* data, std::size_t size) override {
        _outgoing.push(Bytes(data, data + size));
    }

    TransportProfile profile() const override {
        return _profile;
    }

    std::optional<Bytes> tryPopOutgoing() {
        if (_outgoing.empty()) {
            return std::nullopt;
        }
        auto bytes = std::move(_outgoing.front());
        _outgoing.pop();
        return bytes;
    }

    bool isOpen() const {
        return _open;
    }

private:
    IByteSink* _sink = nullptr;
    std::queue<Bytes> _outgoing;
    bool _open = false;
    TransportProfile _profile{
        TransportKind::Mock,
        AxtpWireMode::FramedBinary,
        RpcEncoding::Tlv,
        false,
        false,
        true,
        4096,
    };
};

}  // namespace axtp
