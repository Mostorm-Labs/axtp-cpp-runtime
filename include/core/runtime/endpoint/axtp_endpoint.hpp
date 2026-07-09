#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "core/runtime/broker/basic_broker.hpp"
#include "core/runtime/broker/broker_task.hpp"
#include "core/runtime/core/axtp_core.hpp"
#include "core/runtime/endpoint/endpoint_ports.hpp"
#include "core/runtime/transport/transport.hpp"

namespace axtp {

template <typename Broker = BasicBroker<>>
class AxtpEndpoint {
public:
    explicit AxtpEndpoint(Broker& broker)
        : _broker(broker)
        , _byteSink(*this) {}

    void attachTransport(ITransport& transport) {
        _transport = &transport;
        _core.configure(_transport->profile());
        _transport->bind(_byteSink);
    }

    void detachTransport() {
        _transport = nullptr;
    }

    void poll(std::size_t maxTasks = 8) {
        if (_transport != nullptr) {
            _transport->poll();
        }
        drainCoreEvents();
        _broker.poll(maxTasks);
        drainBrokerResults();
        flushOutbound();
    }

    AxtpCore& core() {
        return _core;
    }

    const AxtpCore& core() const {
        return _core;
    }

    Broker& broker() {
        return _broker;
    }

    const Broker& broker() const {
        return _broker;
    }

    void onTransportBytes(const Byte* data, std::size_t size) {
        _core.byteSink().onBytes(data, size);
    }

    void sendRpcRequest(RpcPayload payload) {
        _core.expectRpcResponse(payload.requestId);
        _core.sendRpcRequest(std::move(payload));
        flushOutbound();
    }

    void sendControlOpen(std::uint16_t controlId) {
        _core.sendControlOpen(controlId);
        flushOutbound();
    }

    void sendRpcSession(RpcPayload payload) {
        _core.sendRpcSession(std::move(payload));
        flushOutbound();
    }

    void sendStream(StreamPayload payload) {
        _core.handleBrokerResult(BrokerResult::streamData(std::move(payload)));
        flushOutbound();
    }

    std::optional<RpcPayload> tryTakeRpcResponse(std::uint32_t requestId) {
        return _core.tryTakeRpcResponse(requestId);
    }

    std::optional<RpcPayload> tryTakeAnyRpcResponse() {
        return _core.tryTakeAnyRpcResponse();
    }

    std::optional<RpcPayload> tryTakeSessionRpc(RpcOp op) {
        return _core.tryTakeSessionRpc(op);
    }

    std::optional<ControlPayload> tryTakeControlNotice(ControlOpcode opcode) {
        return _core.tryTakeControlNotice(opcode);
    }

    void flushOutbound() {
        if (_transport == nullptr) {
            return;
        }
        while (auto bytes = _core.tryPopOutboundBytes()) {
            _transport->sendBytes(bytes->data(), bytes->size());
        }
    }

private:
    void drainCoreEvents() {
        while (auto event = _core.pollEvent()) {
            _broker.submit(BrokerTask::fromCoreEvent(std::move(*event)));
        }
    }

    void drainBrokerResults() {
        while (auto result = _broker.pollResult()) {
            _core.handleBrokerResult(std::move(*result));
        }
    }

    Broker& _broker;
    AxtpCore _core;
    ITransport* _transport = nullptr;
    EndpointByteSink<AxtpEndpoint> _byteSink;
};

}  // namespace axtp
