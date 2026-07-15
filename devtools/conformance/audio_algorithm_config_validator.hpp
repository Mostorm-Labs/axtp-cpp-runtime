#pragma once

#include <nlohmann/json.hpp>
#include <optional>

#include "core/protocol/generated/registry_lookup.h"
#include "core/protocol/model/payload.hpp"

namespace axtp {

// Test-only validator used by the conformance graph.  It is intentionally kept
// outside the installed runtime include tree: product/domain validation belongs
// to the embedding layer, not to the protocol runtime public API.
class AudioAlgorithmConfigValidator {
public:
    AudioAlgorithmConfigValidator()
        : _methodId(RegistryLookup::methodIdByName("audio.setAlgorithmConfig")) {}

    ErrorCode operator()(const RpcPayload& request) const {
        if (!_methodId.has_value() || request.encoding != RpcEncoding::Json ||
            request.methodOrEventId != *_methodId) return ErrorCode::Success;
        try {
            const auto params = nlohmann::json::parse(request.body);
            const auto config = params.find("config");
            if (config == params.end() || !config->is_object()) return ErrorCode::RpcParamMissing;
            const auto noiseSuppression = config->find("noiseSuppression");
            if (noiseSuppression == config->end()) return ErrorCode::Success;
            if (!noiseSuppression->is_object()) return ErrorCode::RpcParamInvalid;
            const auto level = noiseSuppression->find("level");
            if (level == noiseSuppression->end()) return ErrorCode::Success;
            if (!level->is_number_integer() && !level->is_number_unsigned())
                return ErrorCode::RpcParamInvalid;
            // AudioNoiseSuppressionConfig.level in the pinned schema is uint8 [0, 3].
            const auto value = level->get<std::int64_t>();
            return value < 0 || value > 3 ? ErrorCode::OutOfRange : ErrorCode::Success;
        } catch (const std::exception&) {
            return ErrorCode::RpcParamInvalid;
        }
    }

private:
    std::optional<std::uint16_t> _methodId;
};

}  // namespace axtp
