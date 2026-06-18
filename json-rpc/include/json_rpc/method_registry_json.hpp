#pragma once

#include <cstdint>
#include <exception>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "protocol/generated/method_registry.h"

namespace axtp {

class MethodRegistryJson {
public:
    static MethodRegistry fromJson(std::string_view json) {
        MethodRegistry registry;
        const auto parsed = nlohmann::json::parse(json);
        const auto* methods = &parsed;
        if (parsed.is_object()) {
            if (const auto value = parsed.find("methods"); value != parsed.end()) {
                methods = &(*value);
            }
        }
        if (!methods->is_array()) {
            return registry;
        }
        for (const auto& value : *methods) {
            if (!value.is_object()) {
                continue;
            }
            const auto id = value.find("id");
            const auto name = value.find("name");
            if (id == value.end() || name == value.end() || !name->is_string()) {
                continue;
            }
            const auto parsedId = parseId(*id);
            if (!parsedId.has_value()) {
                continue;
            }
            registry.addMethod(*parsedId, name->get<std::string>());
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
    static std::optional<std::uint32_t> parseId(const nlohmann::json& value) {
        if (value.is_number_unsigned()) {
            const auto raw = value.get<std::uint64_t>();
            if (raw <= 0xFFFFFFFFULL) {
                return static_cast<std::uint32_t>(raw);
            }
            return std::nullopt;
        }
        if (value.is_number_integer()) {
            const auto raw = value.get<std::int64_t>();
            if (raw >= 0 && raw <= 0xFFFFFFFFLL) {
                return static_cast<std::uint32_t>(raw);
            }
            return std::nullopt;
        }
        if (value.is_string()) {
            const auto text = value.get<std::string>();
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
