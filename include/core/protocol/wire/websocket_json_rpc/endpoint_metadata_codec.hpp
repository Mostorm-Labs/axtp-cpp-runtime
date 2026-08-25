#pragma once

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

#include "core/protocol/model/endpoint_metadata.hpp"

namespace axtp {

inline EndpointMetadata decodeEndpointMetadata(const nlohmann::json& object) {
    const auto metadata = object.find("m");
    if (metadata == object.end()) {
        return {};
    }
    if (!metadata->is_object()) {
        throw std::invalid_argument("invalid endpoint metadata");
    }

    EndpointMetadata result;
    const auto parseEndpoint = [&](const char* field) -> std::optional<std::string> {
        const auto value = metadata->find(field);
        if (value == metadata->end()) {
            return std::nullopt;
        }
        if (!value->is_string()) {
            throw std::invalid_argument("invalid endpoint metadata field");
        }
        auto endpoint = value->get<std::string>();
        if (endpoint.empty()) {
            throw std::invalid_argument("empty endpoint metadata field");
        }
        return endpoint;
    };

    result.src = parseEndpoint("src");
    result.dst = parseEndpoint("dst");
    return result;
}

inline void addEndpointMetadata(nlohmann::json& object,
                                const EndpointMetadata& metadata) {
    if (!hasEndpointMetadata(metadata)) {
        return;
    }

    auto wireMetadata = nlohmann::json::object();
    const auto addEndpoint = [&](const char* field,
                                 const std::optional<std::string>& endpoint) {
        if (!endpoint.has_value()) {
            return;
        }
        if (endpoint->empty()) {
            throw std::invalid_argument("empty endpoint metadata field");
        }
        wireMetadata[field] = *endpoint;
    };

    addEndpoint("src", metadata.src);
    addEndpoint("dst", metadata.dst);
    object["m"] = std::move(wireMetadata);
}

}  // namespace axtp
