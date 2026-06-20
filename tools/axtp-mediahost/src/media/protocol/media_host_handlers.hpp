#pragma once

#include "media/protocol/media_stream_registry.hpp"
#include "core/runtime/broker/basic_broker.hpp"

namespace axtp::mediahost {

inline void installMediaHostHandlers(BasicBroker<>& broker, MediaStreamRegistry& registry) {
    auto jsonHandler = [&registry](MediaKind kind, bool open, const RpcRequestView& request) {
        const std::string params(request.body.begin(), request.body.end());
        const auto result =
            open ? registry.acceptProducerOpen(kind, params) : registry.close(kind, params);
        RpcResponseData response;
        response.encoding = RpcEncoding::Json;
        response.overrideEncoding = true;
        response.statusCode = result.status;
        response.overrideStatus = true;
        if (result.status == ErrorCode::Success) {
            const auto text = result.body.dump();
            response.body.assign(text.begin(), text.end());
        }
        return response;
    };

    broker.registerRawMethod(static_cast<std::uint16_t>(MethodId::VideoOpenStream),
                             [jsonHandler](const RpcContext&, const RpcRequestView& request) {
                                 return jsonHandler(MediaKind::Video, true, request);
                             });
    broker.registerRawMethod(static_cast<std::uint16_t>(MethodId::AudioOpenStream),
                             [jsonHandler](const RpcContext&, const RpcRequestView& request) {
                                 return jsonHandler(MediaKind::Audio, true, request);
                             });
    broker.registerRawMethod(static_cast<std::uint16_t>(MethodId::VideoCloseStream),
                             [jsonHandler](const RpcContext&, const RpcRequestView& request) {
                                 return jsonHandler(MediaKind::Video, false, request);
                             });
    broker.registerRawMethod(static_cast<std::uint16_t>(MethodId::AudioCloseStream),
                             [jsonHandler](const RpcContext&, const RpcRequestView& request) {
                                 return jsonHandler(MediaKind::Audio, false, request);
                             });
    broker.registerStreamHandler([&registry](const BrokerContext&, const StreamPayload& stream) {
        registry.handleStream(stream);
    });
}

} // namespace axtp::mediahost
