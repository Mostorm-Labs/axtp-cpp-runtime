#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>

#include "generated/registry_lookup.h"
#include "model/payload.hpp"

namespace axtp {

class JsonRpcEncoder {
public:
    Bytes encode(RpcPayload payload) const {
        std::string text;
        switch (payload.op) {
        case RpcOp::Hello:
            text = serializeHello();
            break;
        case RpcOp::Identified:
            text = serializeIdentified(payload);
            break;
        case RpcOp::Event:
            text = serializeEvent(payload);
            break;
        case RpcOp::RequestBatchResponse:
            text = serializeBatchResponse(payload);
            break;
        default:
            text = serializeResponse(payload);
            break;
        }
        return Bytes(text.begin(), text.end());
    }

    static RpcPayload makeHello() {
        RpcPayload payload;
        payload.encoding = RpcEncoding::Json;
        payload.op = RpcOp::Hello;
        payload.bodyEncoding = RpcBodyEncoding::None;
        payload.meta.sourceProtocol = SourceProtocol::JsonRpc;
        return payload;
    }

    static RpcPayload makeIdentified(std::string sid) {
        RpcPayload payload;
        payload.encoding = RpcEncoding::Json;
        payload.op = RpcOp::Identified;
        payload.bodyEncoding = RpcBodyEncoding::None;
        payload.meta.sourceProtocol = SourceProtocol::JsonRpc;
        payload.meta.jsonSid = std::move(sid);
        return payload;
    }

private:
    static std::optional<nlohmann::json> bytesToJson(const Bytes& bytes) {
        if (bytes.empty()) {
            return std::nullopt;
        }
        try {
            const std::string text(bytes.begin(), bytes.end());
            return nlohmann::json::parse(text);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    static const char* errorName(ErrorCode code) {
        const auto* descriptor = RegistryLookup::errorByCode(code);
        return descriptor != nullptr ? descriptor->name : "UNKNOWN_ERROR";
    }

    static nlohmann::json statusObject(ErrorCode code) {
        auto status = nlohmann::json::object();
        status["ok"] = code == ErrorCode::Success;
        status["code"] = static_cast<std::uint16_t>(code);
        if (code != ErrorCode::Success) {
            status["msg"] = errorName(code);
        }
        return status;
    }

    static std::string responseSid(const PayloadMeta& meta) {
        return meta.jsonSid;
    }

    static std::string serializeHello() {
        auto d = nlohmann::json::object();
        d["axtpVersion"] = "1.0.0";
        d["rpcVersion"] = 1;

        auto object = nlohmann::json::object();
        object["sid"] = "";
        object["op"] = static_cast<std::uint8_t>(RpcOp::Hello);
        object["d"] = std::move(d);
        return object.dump();
    }

    static std::string serializeIdentified(const RpcPayload& payload) {
        auto d = nlohmann::json::object();
        d["negotiatedRpcVersion"] = 1;

        auto object = nlohmann::json::object();
        object["sid"] = responseSid(payload.meta);
        object["op"] = static_cast<std::uint8_t>(RpcOp::Identified);
        object["d"] = std::move(d);
        return object.dump();
    }

    static std::string serializeResponse(const RpcPayload& payload) {
        auto d = nlohmann::json::object();
        d["id"] = payload.requestId;

        auto statusCode = payload.statusCode;
        auto result = bytesToJson(payload.body);
        if (statusCode == ErrorCode::Success && !payload.body.empty() && !result.has_value()) {
            statusCode = ErrorCode::RpcBodyDecodeFailed;
        }
        d["status"] = statusObject(statusCode);
        if (statusCode == ErrorCode::Success && result.has_value()) {
            d["result"] = std::move(*result);
        }

        auto object = nlohmann::json::object();
        object["sid"] = responseSid(payload.meta);
        object["op"] = static_cast<std::uint8_t>(RpcOp::RequestResponse);
        object["d"] = std::move(d);
        return object.dump();
    }

    static std::string serializeBatchResponse(const RpcPayload& payload) {
        auto d = nlohmann::json::object();
        d["id"] = payload.requestId;
        d["status"] = statusObject(payload.statusCode);

        auto object = nlohmann::json::object();
        object["sid"] = responseSid(payload.meta);
        object["op"] = static_cast<std::uint8_t>(RpcOp::RequestBatchResponse);
        object["d"] = std::move(d);
        return object.dump();
    }

    static std::string serializeEvent(const RpcPayload& payload) {
        auto d = nlohmann::json::object();
        std::string eventName = payload.meta.jsonMethodOrEventName;
        if (eventName.empty()) {
            const auto* event =
                RegistryLookup::eventById(static_cast<std::uint16_t>(payload.methodOrEventId));
            eventName = event != nullptr ? event->name : "";
        }
        d["event"] = eventName;
        if (auto data = bytesToJson(payload.body)) {
            d["data"] = std::move(*data);
        }

        auto object = nlohmann::json::object();
        object["sid"] = responseSid(payload.meta);
        object["op"] = static_cast<std::uint8_t>(RpcOp::Event);
        object["d"] = std::move(d);
        return object.dump();
    }
};

}  // namespace axtp
