#pragma once

#include <boost/json.hpp>
#include <cstdint>
#include <exception>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "generated/method_registry.h"

namespace axtp {

class MethodRegistryJson {
public:
    static MethodRegistry fromJson(std::string_view json) {
        MethodRegistry registry;
        const auto parsed = boost::json::parse(json);
        const auto* methods = &parsed;
        if (parsed.is_object()) {
            const auto& object = parsed.as_object();
            if (const auto* value = object.if_contains("methods")) {
                methods = value;
            }
        }
        if (!methods->is_array()) {
            return registry;
        }
        for (const auto& value : methods->as_array()) {
            if (!value.is_object()) {
                continue;
            }
            const auto& object = value.as_object();
            const auto* id = object.if_contains("id");
            const auto* name = object.if_contains("name");
            if (id == nullptr || name == nullptr || !name->is_string()) {
                continue;
            }
            const auto parsedId = parseId(*id);
            if (!parsedId.has_value()) {
                continue;
            }
            registry.addMethod(*parsedId, std::string(name->as_string()));
        }
        return registry;
    }

    static MethodRegistry fromFile(std::string_view path) {
        std::ifstream input{std::string(path)};
        if (!input) {
            return {};
        }
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return fromJson(buffer.str());
    }

private:
    static std::optional<std::uint32_t> parseId(const boost::json::value& value) {
        if (value.is_uint64()) {
            const auto raw = value.as_uint64();
            if (raw <= 0xFFFFFFFFULL) {
                return static_cast<std::uint32_t>(raw);
            }
            return std::nullopt;
        }
        if (value.is_int64()) {
            const auto raw = value.as_int64();
            if (raw >= 0 && raw <= 0xFFFFFFFFLL) {
                return static_cast<std::uint32_t>(raw);
            }
            return std::nullopt;
        }
        if (value.is_string()) {
            const std::string text(value.as_string());
            std::size_t offset = 0;
            int base = 10;
            if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
                offset = 2;
                base = 16;
            }
            try {
                const auto parsed = std::stoull(text.substr(offset), nullptr, base);
                if (parsed <= 0xFFFFFFFFULL) {
                    return static_cast<std::uint32_t>(parsed);
                }
            } catch (const std::exception&) {
            }
        }
        return std::nullopt;
    }
};

}  // namespace axtp
