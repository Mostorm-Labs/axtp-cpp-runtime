#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "generated/axtp_error_code_generated.h"
#include "generated/axtp_event_registry_generated.h"
#include "generated/axtp_method_registry_generated.h"

namespace axtp {

class RegistryLookup {
public:
    static std::optional<std::uint16_t> methodIdByName(std::string_view name) {
        for (const auto& descriptor : kMethodRegistry) {
            if (std::string_view(descriptor.name) == name) {
                return descriptor.id;
            }
        }
        return std::nullopt;
    }

    static const MethodDescriptor* methodById(std::uint16_t id) {
        for (const auto& descriptor : kMethodRegistry) {
            if (descriptor.id == id) {
                return &descriptor;
            }
        }
        return nullptr;
    }

    static std::optional<std::uint16_t> eventIdByName(std::string_view name) {
        for (const auto& descriptor : kEventRegistry) {
            if (std::string_view(descriptor.name) == name) {
                return descriptor.id;
            }
        }
        return std::nullopt;
    }

    static const EventDescriptor* eventById(std::uint16_t id) {
        for (const auto& descriptor : kEventRegistry) {
            if (descriptor.id == id) {
                return &descriptor;
            }
        }
        return nullptr;
    }

    static const ErrorDescriptor* errorByCode(ErrorCode code) {
        return errorByCode(static_cast<std::uint16_t>(code));
    }

    static const ErrorDescriptor* errorByCode(std::uint16_t code) {
        for (const auto& descriptor : kErrorRegistry) {
            if (descriptor.id == code) {
                return &descriptor;
            }
        }
        return nullptr;
    }
};

} // namespace axtp
