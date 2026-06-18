#pragma once

#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "axtp.hpp"
#include "media/model/format.hpp"
#include "media/model/media_types.hpp"

namespace axtp::mediahost {

inline Bytes bytesFromString(std::string_view text) {
    return Bytes(text.begin(), text.end());
}

inline const char* errorName(ErrorCode code) {
    const auto* descriptor = RegistryLookup::errorByCode(code);
    return descriptor != nullptr ? descriptor->name : "UNKNOWN_ERROR";
}

inline std::string
jsonStringOr(const nlohmann::json& object, const char* name, std::string fallback) {
    const auto it = object.find(name);
    if (it != object.end() && it->is_string()) {
        return it->get<std::string>();
    }
    return fallback;
}

inline std::uint32_t
jsonU32Or(const nlohmann::json& object, const char* name, std::uint32_t fallback) {
    const auto it = object.find(name);
    if (it == object.end()) {
        return fallback;
    }
    try {
        if (it->is_number_unsigned()) {
            const auto value = it->get<std::uint64_t>();
            return value <= std::numeric_limits<std::uint32_t>::max()
                       ? static_cast<std::uint32_t>(value)
                       : fallback;
        }
        if (it->is_number_integer()) {
            const auto value = it->get<std::int64_t>();
            return value >= 0 && static_cast<std::uint64_t>(value) <=
                                     std::numeric_limits<std::uint32_t>::max()
                       ? static_cast<std::uint32_t>(value)
                       : fallback;
        }
    } catch (const std::exception&) {
    }
    return fallback;
}

} // namespace axtp::mediahost
