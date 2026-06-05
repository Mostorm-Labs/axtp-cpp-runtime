#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "generated/axtp_method_registry_generated.h"

namespace axtp {

class MethodRegistry {
public:
    struct Entry {
        std::uint32_t id = 0;
        std::string name;
    };

    void addMethod(std::uint32_t methodId, std::string methodName) {
        auto existing = idToName_.find(methodId);
        if (existing != idToName_.end()) {
            nameToId_.erase(existing->second);
        }
        auto existingName = nameToId_.find(methodName);
        if (existingName != nameToId_.end()) {
            idToName_.erase(existingName->second);
        }
        nameToId_[methodName] = methodId;
        idToName_[methodId] = std::move(methodName);
    }

    std::optional<std::uint32_t> findMethodId(std::string_view methodName) const {
        const auto it = nameToId_.find(std::string(methodName));
        if (it == nameToId_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    std::optional<std::string_view> findMethodName(std::uint32_t methodId) const {
        const auto it = idToName_.find(methodId);
        if (it == idToName_.end()) {
            return std::nullopt;
        }
        return std::string_view(it->second);
    }

    bool containsMethod(std::uint32_t methodId) const {
        return idToName_.find(methodId) != idToName_.end();
    }

    bool containsMethod(std::string_view methodName) const {
        return nameToId_.find(std::string(methodName)) != nameToId_.end();
    }

    std::vector<Entry> entries() const {
        std::vector<Entry> out;
        out.reserve(idToName_.size());
        for (const auto& item : idToName_) {
            out.push_back(Entry{item.first, item.second});
        }
        return out;
    }

    static MethodRegistry fromGeneratedDefaults() {
        MethodRegistry registry;
        for (const auto& method : kMethodRegistry) {
            registry.addMethod(method.id, method.name);
        }
        return registry;
    }

private:
    struct TransparentLess {
        using is_transparent = void;
        bool operator()(const std::string& lhs, const std::string& rhs) const {
            return lhs < rhs;
        }
    };

    std::map<std::string, std::uint32_t, TransparentLess> nameToId_;
    std::map<std::uint32_t, std::string> idToName_;
};

} // namespace axtp
