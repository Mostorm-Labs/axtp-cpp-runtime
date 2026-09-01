#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
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
        _core.resetResponseTracking();
        _core.configure(_transport->profile());
        _transport->bind(_byteSink);
        _core.setIngressReplyTargetProvider([this]() {
            return _transport != nullptr ? _transport->currentReplyTarget() : 0;
        });
    }

    void detachTransport() {
        _transport = nullptr;
        _core.resetResponseTracking();
        _core.setIngressReplyTargetProvider({});
    }

    void setIngressTokenProvider(typename AxtpCore::IngressTokenProvider provider) {
        _core.setIngressTokenProvider(std::move(provider));
    }

    void poll(std::size_t maxTasks = 8) {
        if (_transport != nullptr) {
            _transport->poll();
        }
        progress(maxTasks);
    }

    void progress(std::size_t maxTasks = 8) {
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

    bool sendRpcRequest(RpcPayload payload) {
        if (!_core.expectRpcResponse(payload.requestId)) {
            return false;
        }
        _core.sendRpcRequest(std::move(payload));
        flushOutbound();
        return true;
    }

    void sendControlOpen(std::uint16_t controlId) {
        _core.sendControlOpen(controlId);
        flushOutbound();
    }

    void sendControlHeartbeat(std::uint16_t controlId) {
        _core.sendControlHeartbeat(controlId);
        flushOutbound();
    }

    std::optional<std::uint32_t> negotiatedHeartbeatIntervalMs() const {
        return _core.negotiatedHeartbeatIntervalMs();
    }

    std::uint64_t inboundActivityGeneration() const {
        return _core.inboundActivityGeneration();
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

    void abandonRpcResponse(std::uint32_t requestId) {
        _core.abandonRpcResponse(requestId);
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

    std::optional<ControlPayload> tryTakeControlNotice(
        ControlOpcode opcode,
        std::uint16_t controlId) {
        return _core.tryTakeControlNotice(opcode, controlId);
    }

    void flushOutbound() {
        if (_transport == nullptr) {
            return;
        }
        while (auto packet = _core.tryPopOutboundPacket()) {
            if (packet->replyTarget != 0) {
                _transport->sendBytesTo(
                    packet->replyTarget, packet->bytes.data(), packet->bytes.size());
            } else {
                _transport->sendBytes(packet->bytes.data(), packet->bytes.size());
            }
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
