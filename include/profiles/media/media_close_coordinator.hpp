#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "profiles/media/media_stream_registry.hpp"
#include "stream/stream_coordinator.hpp"

namespace axtp::mediahost {

class MediaCloseCoordinator {
public:
    MediaCloseCoordinator(std::string sid,
                          std::chrono::milliseconds requestTimeout,
                          LogFn log = {})
        : _coordinator(std::move(sid), requestTimeout, std::move(log)) {}

    void setSid(std::string sid) {
        _coordinator.setSid(std::move(sid));
    }

    template <typename Endpoint> void sendClose(Endpoint& endpoint, const ActiveMediaStream& stream) {
        _coordinator.sendClose(endpoint,
                               axtp::stream::StreamCloseRequest{
                                   methodIdFor(stream.kind),
                                   methodNameFor(stream.kind),
                                   stream.streamId,
                                   stream.source,
                               });
    }

    template <typename Endpoint> void poll(Endpoint& endpoint) {
        _coordinator.poll(endpoint);
    }

    std::size_t pendingCount() const {
        return _coordinator.pendingCount();
    }

    static nlohmann::json closeParamsFor(std::uint32_t streamId) {
        return axtp::stream::StreamCloseCoordinator::closeParamsFor(streamId);
    }

private:
    static std::uint16_t methodIdFor(MediaKind kind) {
        return static_cast<std::uint16_t>(kind == MediaKind::Video ? MethodId::VideoCloseStream
                                                                   : MethodId::AudioCloseStream);
    }

    static const char* methodNameFor(MediaKind kind) {
        return kind == MediaKind::Video ? "video.closeStream" : "audio.closeStream";
    }

    axtp::stream::StreamCloseCoordinator _coordinator;
};

} // namespace axtp::mediahost
