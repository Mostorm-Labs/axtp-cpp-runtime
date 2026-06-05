#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <utility>

#include "axtp.hpp"

namespace axtp::sdk {

class AxtpServer {
public:
    using RawMethodHandler = std::function<Bytes(const RpcPayload&)>;

    AxtpServer()
        : _endpoint(std::make_unique<AxtpEndpoint<BasicBroker<>>>(_broker)) {}

    void attachTransport(std::unique_ptr<ITransport> transport) {
        close();
        _transport = std::move(transport);
        _endpoint = std::make_unique<AxtpEndpoint<BasicBroker<>>>(_broker);
        if (_transport != nullptr) {
            _endpoint->attachTransport(*_transport);
            _transport->open();
        }
    }

    void close() {
        if (_transport != nullptr) {
            _transport->close();
        }
    }

    void poll() {
        if (_endpoint != nullptr) {
            _endpoint->poll();
        }
    }

    void onRaw(std::uint32_t methodId, RawMethodHandler handler) {
        _broker.registerRawMethod(
            methodId,
            [handler = std::move(handler)](const RpcContext&, const RpcRequestView& request) {
                RpcPayload payload;
                payload.encoding = request.encoding;
                payload.op = RpcOp::Request;
                payload.requestId = request.requestId;
                payload.methodOrEventId = request.methodId;
                payload.body = request.body;
                RpcResponseData response;
                response.body = handler(payload);
                return response;
            });
    }

    void onJson(std::string_view methodName, JsonRpcHandler handler) {
        _broker.registerJsonMethod(methodName, std::move(handler));
    }

    void onTlv(std::string_view methodName, TlvRpcHandler handler) {
        _broker.registerTlvMethod(methodName, std::move(handler));
    }

    void emitRaw(RpcPayload payload) {
        _lastEvent = std::move(payload);
    }

    const RpcPayload& lastEvent() const {
        return _lastEvent;
    }

private:
    std::unique_ptr<ITransport> _transport;
    BasicBroker<> _broker;
    std::unique_ptr<AxtpEndpoint<BasicBroker<>>> _endpoint;
    RpcPayload _lastEvent;
};

}  // namespace axtp::sdk
