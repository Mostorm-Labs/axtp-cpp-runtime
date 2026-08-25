#pragma once

#include <optional>
#include <string>

namespace axtp {

struct EndpointMetadata {
    std::optional<std::string> src;
    std::optional<std::string> dst;
};

inline bool hasEndpointMetadata(const EndpointMetadata& metadata) {
    return metadata.src.has_value() || metadata.dst.has_value();
}

inline EndpointMetadata responseEndpointMetadata(const EndpointMetadata& request) {
    return EndpointMetadata{request.dst, request.src};
}

}  // namespace axtp
