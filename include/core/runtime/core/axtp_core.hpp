#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <optional>
#include <queue>
#include <sstream>
#include <utility>

#include "core/runtime/broker/broker_result.hpp"
#include "core/protocol/session/control_session.hpp"
#include "core/runtime/core/core_event.hpp"
#include "core/protocol/wire/inbound_processor.hpp"
#include "core/protocol/wire/websocket_json_rpc/outbound/json_rpc_encoder.hpp"
#include "core/protocol/wire/outbound_processor.hpp"
#include "core/protocol/session/pending_call_table.hpp"
#include "core/protocol/session/session_context.hpp"
#include "core/protocol/session/stream_session.hpp"
#include "core/support/io/byte_sink.hpp"
#include "core/support/io/byte_writer_sink.hpp"
#include "core/runtime/transport/transport_profile.hpp"

namespace axtp {

class AxtpCore {
public:
    using JsonRpcNameLookup = InboundProcessor::NameLookup;
    using IngressTokenProvider = std::function<std::uint64_t()>;
    using IngressReplyTargetProvider = std::function<std::uint64_t()>;

    struct OutboundPacket {
        Bytes bytes;
        std::uint64_t replyTarget = 0;
    };

    AxtpCore()
        : _byteSink(*this)
        , _payloadSink(*this)
        , _byteWriter(*this)
        , _inbound(_payloadSink)
        , _outbound(_byteWriter) {}

    void configure(const TransportProfile& profile) {
        _transportProfile = profile;
        _inbound.setWireMode(profile.wireMode);
        _outbound.setWireMode(profile.wireMode);
        if (profile.preferredFrameSize > 0) {
            _outbound.setMaxFrameSize(profile.preferredFrameSize);
        }
    }

    void setRequestedHeartbeatInterval(std::uint32_t intervalMs) {
        _controlSession.setRequestedHeartbeatInterval(intervalMs);
    }

    IByteSink& byteSink() {
        return _byteSink;
    }

    void setJsonRpcMethodLookup(JsonRpcNameLookup lookup) {
        _inbound.setJsonRpcMethodLookup(std::move(lookup));
    }

    void setJsonRpcEventLookup(JsonRpcNameLookup lookup) {
        _inbound.setJsonRpcEventLookup(std::move(lookup));
    }

    // The provider is invoked while a payload is still at the Core ingress
    // boundary (before it is deferred into the broker task queue).  The value
    // is internal provenance and is never serialized on the wire.
    void setIngressTokenProvider(IngressTokenProvider provider) {
        _ingressTokenProvider = std::move(provider);
    }

    void setIngressReplyTargetProvider(IngressReplyTargetProvider provider) {
        _ingressReplyTargetProvider = std::move(provider);
    }

    std::optional<CoreEvent> pollEvent() {
        if (_events.empty()) {
            return std::nullopt;
        }
        auto event = std::move(_events.front());
        _events.pop();
        return event;
    }

    void handleBrokerResult(BrokerResult result) {
        const auto previousReplyTarget = _outboundReplyTarget;
        if (result.type == BrokerResultType::RpcResponse ||
            result.type == BrokerResultType::RpcError) {
            _outboundReplyTarget = result.rpc.meta.replyTarget;
        }
        switch (result.type) {
        case BrokerResultType::RpcResponse:
            _outbound.sendRpcResponse(std::move(result.rpc));
            break;
        case BrokerResultType::RpcError:
            _outbound.sendRpcError(std::move(result.rpc));
            break;
        case BrokerResultType::Event:
            _outbound.sendEvent(std::move(result.rpc));
            break;
        case BrokerResultType::StreamData:
        case BrokerResultType::StreamClose:
            _outbound.sendStream(std::move(result.stream));
            break;
        case BrokerResultType::Noop:
            break;
        }
        _outboundReplyTarget = previousReplyTarget;
    }

    bool expectRpcResponse(std::uint32_t requestId) {
        return _pendingCalls.expect(requestId);
    }

    void abandonRpcResponse(std::uint32_t requestId) {
        _pendingCalls.abandon(requestId);
    }

    std::optional<RpcPayload> tryTakeRpcResponse(std::uint32_t requestId) {
        return _pendingCalls.tryTakeResolved(requestId);
    }

    std::optional<RpcPayload> tryTakeAnyRpcResponse() {
        return _pendingCalls.tryTakeAnyResolved();
    }

    void resetResponseTracking() {
        _pendingCalls.reset();
    }

    std::optional<RpcPayload> tryTakeSessionRpc(RpcOp op) {
        const auto count = _sessionRpcs.size();
        for (std::size_t index = 0; index < count; ++index) {
            auto payload = std::move(_sessionRpcs.front());
            _sessionRpcs.pop();
            if (payload.op == op) {
                return payload;
            }
            _sessionRpcs.push(std::move(payload));
        }
        return std::nullopt;
    }

    std::optional<ControlPayload> tryTakeControlNotice(ControlOpcode opcode) {
        const auto count = _controlNotices.size();
        for (std::size_t index = 0; index < count; ++index) {
            auto payload = std::move(_controlNotices.front());
            _controlNotices.pop();
            if (payload.opcode == opcode) {
                return payload;
            }
            _controlNotices.push(std::move(payload));
        }
        return std::nullopt;
    }

    // Variant used by request/response helpers that must not consume an ACK
    // belonging to another in-flight control request.  Non-matching notices
    // stay queued in their original order.
    std::optional<ControlPayload> tryTakeControlNotice(
        ControlOpcode opcode,
        std::uint16_t controlId) {
        const auto count = _controlNotices.size();
        for (std::size_t index = 0; index < count; ++index) {
            auto payload = std::move(_controlNotices.front());
            _controlNotices.pop();
            if (payload.opcode == opcode && payload.controlId == controlId) {
                return payload;
            }
            _controlNotices.push(std::move(payload));
        }
        return std::nullopt;
    }

    std::optional<Bytes> tryPopOutboundBytes() {
        auto packet = tryPopOutboundPacket();
        if (!packet.has_value()) {
            return std::nullopt;
        }
        return std::move(packet->bytes);
    }

    std::optional<OutboundPacket> tryPopOutboundPacket() {
        if (_outboundBytes.empty()) {
            return std::nullopt;
        }
        auto bytes = std::move(_outboundBytes.front());
        _outboundBytes.pop();
        return bytes;
    }

    bool controlSessionOpen() const {
        return _controlSession.isOpen();
    }

    void sendControlOpen(std::uint16_t controlId) {
        _outbound.sendControl(_controlSession.makeOpen(controlId));
    }

    void sendControlHeartbeat(std::uint16_t controlId) {
        _outbound.sendControl(_controlSession.makeHeartbeat(controlId));
    }

    std::optional<std::uint32_t> negotiatedHeartbeatIntervalMs() const {
        return _controlSession.negotiatedHeartbeatIntervalMs();
    }

    // Monotonic generation of successfully decoded inbound control/RPC/stream
    // payloads.  Transport consumers can use this as liveness evidence while
    // malformed or filtered reports remain invisible at this layer.
    std::uint64_t inboundActivityGeneration() const {
        return _inboundActivityGeneration;
    }

    void sendRpcRequest(RpcPayload payload) {
        _outbound.sendRpcRequest(std::move(payload));
    }

    void sendRpcSession(RpcPayload payload) {
        _outbound.sendRpc(std::move(payload));
    }

private:
    class ByteSinkPort final : public IByteSink {
    public:
        explicit ByteSinkPort(AxtpCore& core)
            : _core(core) {}

        void onBytes(const Byte* data, std::size_t size) override {
            _core.handleBytes(data, size);
        }

    private:
        AxtpCore& _core;
    };

    class PayloadSinkPort final : public IPayloadSink {
    public:
        explicit PayloadSinkPort(AxtpCore& core)
            : _core(core) {}

        void onControl(ControlPayload payload) override {
            _core.handleControl(std::move(payload));
        }

        void onRpc(RpcPayload payload) override {
            _core.handleRpc(std::move(payload));
        }

        void onStream(StreamPayload payload) override {
            _core.handleStream(std::move(payload));
        }

    private:
        AxtpCore& _core;
    };

    class ByteWriterPort final : public IByteWriter {
    public:
        explicit ByteWriterPort(AxtpCore& core)
            : _core(core) {}

        void writeBytes(const Byte* data, std::size_t size) override {
            _core.enqueueOutboundBytes(data, size);
        }

    private:
        AxtpCore& _core;
    };

    void handleBytes(const Byte* data, std::size_t size) {
        _inbound.onBytes(data, size);
    }

    void enqueueOutboundBytes(const Byte* data, std::size_t size) {
        _outboundBytes.push({Bytes(data, data + size), _outboundReplyTarget});
    }

    void handleControl(ControlPayload payload) {
        ++_inboundActivityGeneration;
        const auto opcode = payload.opcode;
        const auto notice = payload;
        auto response = _controlSession.handle(std::move(payload));
        _controlNotices.push(notice);
        if (response.has_value()) {
            _outbound.sendControl(std::move(*response));
            if (opcode == ControlOpcode::Open && _controlSession.isOpen()) {
                _outbound.sendRpc(JsonRpcEncoder::makeHello());
            }
        }
    }

    void handleRpc(RpcPayload payload) {
        ++_inboundActivityGeneration;
        if (payload.op == RpcOp::Identify || payload.op == RpcOp::Reidentify) {
            const auto randomSeed = payload.meta.hasRandomSeed ? payload.meta.randomSeed : 0;
            const auto sid = makeSessionId(randomSeed);
            _sessionRpcs.push(std::move(payload));
            _outbound.sendRpc(JsonRpcEncoder::makeIdentified(sid));
            return;
        }
        if (payload.op == RpcOp::Hello || payload.op == RpcOp::Identified) {
            _sessionRpcs.push(std::move(payload));
            return;
        }
        if (payload.op == RpcOp::Request) {
            captureReplyTarget(payload.meta);
            _events.push(CoreEvent::rpcRequest(std::move(payload)));
            return;
        }
        if (payload.op == RpcOp::Event) {
            _events.push(CoreEvent::rpcEvent(std::move(payload)));
            return;
        }
        if (payload.op == RpcOp::RequestResponse) {
            if (payload.meta.localGeneratedResponse) {
                _outbound.sendRpcResponse(std::move(payload));
                return;
            }
            if (payload.requestId != 0 && !_pendingCalls.isPending(payload.requestId)) {
                // A response for a timed-out, cancelled, duplicate, or
                // otherwise unknown call is intentionally consumed here.  Do
                // not echo it back on JSON-RPC: that turns a late response
                // into a new wire transaction and can poison a later call.
                return;
            }
            _pendingCalls.resolve(payload.requestId, std::move(payload));
            return;
        }
        if (payload.op == RpcOp::RequestBatchResponse) {
            if (payload.meta.localGeneratedResponse) {
                _outbound.sendRpcResponse(std::move(payload));
                return;
            }
            if (payload.requestId != 0 && !_pendingCalls.isPending(payload.requestId)) {
                return;
            }
            _pendingCalls.resolve(payload.requestId, std::move(payload));
        }
    }

    void handleStream(StreamPayload payload) {
        ++_inboundActivityGeneration;
        if (_ingressTokenProvider) {
            try {
                payload.meta.ingressToken = _ingressTokenProvider();
            } catch (...) {
                // A provenance provider is diagnostic/lifecycle metadata and
                // must never make a valid media payload fail at the wire
                // boundary.  Leave the token unset on provider failure.
                payload.meta.ingressToken = 0;
            }
        }
        _streamSession.handle(payload);
        _events.push(CoreEvent::streamData(std::move(payload)));
    }

    void captureReplyTarget(PayloadMeta& meta) {
        if (!_ingressReplyTargetProvider) {
            return;
        }
        try {
            meta.replyTarget = _ingressReplyTargetProvider();
        } catch (...) {
            meta.replyTarget = 0;
        }
    }

    ByteSinkPort _byteSink;
    PayloadSinkPort _payloadSink;
    ByteWriterPort _byteWriter;
    InboundProcessor _inbound;
    OutboundProcessor _outbound;
    ControlSession _controlSession;
    StreamSession _streamSession;
    PendingCallTable _pendingCalls;
    TransportProfile _transportProfile;
    std::queue<CoreEvent> _events;
    std::queue<RpcPayload> _sessionRpcs;
    std::queue<ControlPayload> _controlNotices;
    std::queue<OutboundPacket> _outboundBytes;
    std::uint64_t _outboundReplyTarget = 0;
    std::uint32_t _nextSessionId = 1;
    std::uint64_t _inboundActivityGeneration = 0;
    IngressTokenProvider _ingressTokenProvider;
    IngressReplyTargetProvider _ingressReplyTargetProvider;

    std::string makeSessionId(std::uint32_t randomSeed) {
        auto mixed = randomSeed ^ (_nextSessionId++ * 0x9E3779B9U);
        if (mixed == 0) {
            mixed = _nextSessionId;
        }
        std::ostringstream out;
        out << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << mixed;
        return out.str();
    }
};

}  // namespace axtp
