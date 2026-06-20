#pragma once

#include <cstddef>
#include <cstdint>
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

    IByteSink& byteSink() {
        return _byteSink;
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
    }

    void expectRpcResponse(std::uint32_t requestId) {
        _pendingCalls.expect(requestId);
    }

    std::optional<RpcPayload> tryTakeRpcResponse(std::uint32_t requestId) {
        return _pendingCalls.tryTakeResolved(requestId);
    }

    std::optional<RpcPayload> tryTakeAnyRpcResponse() {
        return _pendingCalls.tryTakeAnyResolved();
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

    std::optional<Bytes> tryPopOutboundBytes() {
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
        _outboundBytes.push(Bytes(data, data + size));
    }

    void handleControl(ControlPayload payload) {
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
        if (payload.op == RpcOp::Identify || payload.op == RpcOp::Reidentify) {
            if (!payload.meta.hasRandomSeed) {
                _sessionRpcs.push(std::move(payload));
                return;
            }
            const auto sid = makeSessionId(payload.meta.randomSeed);
            _sessionRpcs.push(std::move(payload));
            _outbound.sendRpc(JsonRpcEncoder::makeIdentified(sid));
            return;
        }
        if (payload.op == RpcOp::Hello || payload.op == RpcOp::Identified) {
            _sessionRpcs.push(std::move(payload));
            return;
        }
        if (payload.op == RpcOp::Request) {
            _events.push(CoreEvent::rpcRequest(std::move(payload)));
            return;
        }
        if (payload.op == RpcOp::Event) {
            _events.push(CoreEvent::rpcEvent(std::move(payload)));
            return;
        }
        if (payload.op == RpcOp::RequestResponse) {
            if (!_pendingCalls.isPending(payload.requestId) &&
                payload.meta.sourceProtocol == SourceProtocol::JsonRpc) {
                _outbound.sendRpcResponse(std::move(payload));
                return;
            }
            _pendingCalls.resolve(payload.requestId, std::move(payload));
            return;
        }
        if (payload.op == RpcOp::RequestBatchResponse &&
            payload.meta.sourceProtocol == SourceProtocol::JsonRpc) {
            _outbound.sendRpcResponse(std::move(payload));
        }
    }

    void handleStream(StreamPayload payload) {
        _streamSession.handle(payload);
        _events.push(CoreEvent::streamData(std::move(payload)));
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
    std::queue<Bytes> _outboundBytes;
    std::uint32_t _nextSessionId = 1;

    std::string makeSessionId(std::uint32_t randomSeed) {
        auto mixed = randomSeed ^ (_nextSessionId++ * 0x9E3779B9U);
        if (mixed == 0) {
            mixed = _nextSessionId;
        }
        std::ostringstream out;
        out << std::hex << std::setw(8) << std::setfill('0') << mixed;
        return out.str();
    }
};

}  // namespace axtp
