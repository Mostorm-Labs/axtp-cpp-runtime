#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "core/protocol/model/payload.hpp"

namespace axtp {

class RpcClientSession {
public:
    nlohmann::json acceptHello(const nlohmann::json& hello,
                               std::uint32_t randomSeed,
                               std::string eventMasks = "") {
        if (!hello.is_object() || hello.value("op", -1) != static_cast<int>(RpcOp::Hello) ||
            !hello.contains("d") || !hello.at("d").is_object()) {
            throw std::invalid_argument("invalid HELLO shape");
        }
        _observedAxtpVersion.reset();
        if (const auto version = hello.at("d").find("axtpVersion"); version != hello.at("d").end()) {
            _observedAxtpVersion = version->is_string() ? version->get<std::string>() : version->dump();
        }
        _helloAccepted = true;  // Diagnostic version is deliberately not a compatibility gate.
        return {{"sid", ""},
                {"op", static_cast<int>(RpcOp::Identify)},
                {"d", {{"randomSeed", randomSeed}, {"eventMasks", std::move(eventMasks)}}}};
    }

    void acceptIdentified(const nlohmann::json& identified) {
        if (!_helloAccepted || !identified.is_object() ||
            identified.value("op", -1) != static_cast<int>(RpcOp::Identified) ||
            !identified.contains("sid") || !identified.at("sid").is_string() ||
            identified.at("sid").get<std::string>().empty()) {
            throw std::invalid_argument("invalid IDENTIFIED shape/state");
        }
        _sid = identified.at("sid").get<std::string>();
    }

    nlohmann::json makeReidentify(std::string eventMasks = "") const {
        if (_sid.empty()) throw std::logic_error("client session is not identified");
        auto d = nlohmann::json::object();
        if (!eventMasks.empty()) d["eventMasks"] = std::move(eventMasks);
        return {{"sid", _sid}, {"op", static_cast<int>(RpcOp::Reidentify)}, {"d", std::move(d)}};
    }

    const std::optional<std::string>& observedAxtpVersion() const { return _observedAxtpVersion; }
    const std::string& sid() const { return _sid; }

private:
    bool _helloAccepted = false;
    std::optional<std::string> _observedAxtpVersion;
    std::string _sid;
};

}  // namespace axtp
