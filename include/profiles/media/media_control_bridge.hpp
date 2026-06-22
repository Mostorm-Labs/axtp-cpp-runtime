#pragma once

#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/protocol/generated/axtp_ids_generated.h"
#include "core/runtime/broker/basic_broker.hpp"
#include "profiles/media/media_types.hpp"

namespace axtp::mediahost {

using MediaControlResult = OpenStreamResult;

struct MediaControlCallbacks {
    std::function<MediaControlResult(MediaKind, nlohmann::json)> openStream;
    std::function<MediaControlResult(MediaKind, nlohmann::json)> closeStream;
    std::function<MediaControlResult(nlohmann::json)> requestKeyFrame;
    std::function<MediaControlResult(std::uint16_t, const char*, nlohmann::json)> forwardRpc;
};

namespace detail {

inline Bytes mediaControlBytesFromString(std::string_view text) {
    return Bytes(text.begin(), text.end());
}

inline nlohmann::json mediaControlJsonFromBytesOrObject(const Bytes& bytes) {
    if (bytes.empty()) {
        return nlohmann::json::object();
    }
    try {
        auto parsed = nlohmann::json::parse(std::string(bytes.begin(), bytes.end()));
        return parsed.is_object() ? parsed : nlohmann::json::object();
    } catch (const std::exception&) {
        return nlohmann::json::object();
    }
}

inline MediaControlResult mediaControlNotSupportedResult() {
    return {ErrorCode::NotSupported, nlohmann::json::object()};
}

inline RpcResponseData makeMediaControlResponse(const MediaControlResult& result) {
    RpcResponseData response;
    response.encoding = RpcEncoding::Json;
    response.overrideEncoding = true;
    response.statusCode = result.status;
    response.overrideStatus = true;
    if (!result.body.is_null()) {
        response.body = mediaControlBytesFromString(result.body.dump());
    }
    return response;
}

inline void registerMediaControlHandler(BasicBroker<>& broker,
                                        MethodId id,
                                        const char* name,
                                        MediaControlCallbacks callbacks) {
    broker.registerRawMethod(static_cast<std::uint16_t>(id),
                             [id, name, callbacks = std::move(callbacks)](
                                 const RpcContext&, const RpcRequestView& request) {
                                 const auto params =
                                     mediaControlJsonFromBytesOrObject(request.body);
                                 MediaControlResult result;
                                 switch (id) {
                                 case MethodId::VideoOpenStream:
                                     result = callbacks.openStream
                                         ? callbacks.openStream(MediaKind::Video, params)
                                         : mediaControlNotSupportedResult();
                                     break;
                                 case MethodId::AudioOpenStream:
                                     result = callbacks.openStream
                                         ? callbacks.openStream(MediaKind::Audio, params)
                                         : mediaControlNotSupportedResult();
                                     break;
                                 case MethodId::VideoCloseStream:
                                     result = callbacks.closeStream
                                         ? callbacks.closeStream(MediaKind::Video, params)
                                         : mediaControlNotSupportedResult();
                                     break;
                                 case MethodId::AudioCloseStream:
                                     result = callbacks.closeStream
                                         ? callbacks.closeStream(MediaKind::Audio, params)
                                         : mediaControlNotSupportedResult();
                                     break;
                                 case MethodId::VideoRequestKeyFrame:
                                     result = callbacks.requestKeyFrame
                                         ? callbacks.requestKeyFrame(params)
                                         : mediaControlNotSupportedResult();
                                     break;
                                 default:
                                     result = callbacks.forwardRpc
                                         ? callbacks.forwardRpc(
                                               static_cast<std::uint16_t>(id), name, params)
                                         : mediaControlNotSupportedResult();
                                     break;
                                 }
                                 return makeMediaControlResponse(result);
                             });
}

} // namespace detail

inline void installMediaControlBridge(BasicBroker<>& broker, MediaControlCallbacks callbacks) {
    detail::registerMediaControlHandler(broker, MethodId::DeviceGetInfo, "device.getInfo", callbacks);
    detail::registerMediaControlHandler(
        broker, MethodId::VideoOpenStream, "video.openStream", callbacks);
    detail::registerMediaControlHandler(
        broker, MethodId::AudioOpenStream, "audio.openStream", callbacks);
    detail::registerMediaControlHandler(
        broker, MethodId::VideoCloseStream, "video.closeStream", callbacks);
    detail::registerMediaControlHandler(
        broker, MethodId::AudioCloseStream, "audio.closeStream", callbacks);
    detail::registerMediaControlHandler(
        broker, MethodId::VideoGetStreamState, "video.getStreamState", callbacks);
    detail::registerMediaControlHandler(
        broker, MethodId::AudioGetStreamState, "audio.getStreamState", callbacks);
    detail::registerMediaControlHandler(
        broker, MethodId::VideoGetStreamCapabilities, "video.getStreamCapabilities", callbacks);
    detail::registerMediaControlHandler(
        broker, MethodId::AudioGetStreamCapabilities, "audio.getStreamCapabilities", callbacks);
    detail::registerMediaControlHandler(
        broker, MethodId::VideoRequestKeyFrame, "video.requestKeyFrame", std::move(callbacks));
}

} // namespace axtp::mediahost
