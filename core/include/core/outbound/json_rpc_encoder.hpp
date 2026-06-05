#pragma once

#include <boost/json.hpp>
#include <cstdint>
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
        payload.bodyEncoding = RpcBodyEncoding::RawBytes;
        payload.meta.sourceProtocol = SourceProtocol::JsonRpc;
        return payload;
    }

    static RpcPayload makeIdentified(std::string sid) {
        RpcPayload payload;
        payload.encoding = RpcEncoding::Json;
        payload.op = RpcOp::Identified;
        payload.bodyEncoding = RpcBodyEncoding::RawBytes;
        payload.meta.sourceProtocol = SourceProtocol::JsonRpc;
        payload.meta.jsonSid = std::move(sid);
        return payload;
    }

private:
    static std::optional<boost::json::value> bytesToJson(const Bytes& bytes) {
        if (bytes.empty()) {
            return std::nullopt;
        }
        try {
            const std::string text(bytes.begin(), bytes.end());
            return boost::json::parse(text);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    static const char* errorName(ErrorCode code) {
        const auto* descriptor = RegistryLookup::errorByCode(code);
        return descriptor != nullptr ? descriptor->name : "UNKNOWN_ERROR";
    }

    static boost::json::object statusObject(ErrorCode code) {
        boost::json::object status;
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
        boost::json::object d;
        d["axtpVersion"] = "1.0.0";
        d["rpcVersion"] = 1;

        boost::json::object object;
        object["sid"] = "";
        object["op"] = static_cast<std::uint8_t>(RpcOp::Hello);
        object["d"] = std::move(d);
        return boost::json::serialize(object);
    }

    static std::string serializeIdentified(const RpcPayload& payload) {
        boost::json::object d;
        d["negotiatedRpcVersion"] = 1;

        boost::json::object object;
        object["sid"] = responseSid(payload.meta);
        object["op"] = static_cast<std::uint8_t>(RpcOp::Identified);
        object["d"] = std::move(d);
        return boost::json::serialize(object);
    }

    static std::string serializeResponse(const RpcPayload& payload) {
        boost::json::object d;
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

        boost::json::object object;
        object["sid"] = responseSid(payload.meta);
        object["op"] = static_cast<std::uint8_t>(RpcOp::RequestResponse);
        object["d"] = std::move(d);
        return boost::json::serialize(object);
    }

    static std::string serializeBatchResponse(const RpcPayload& payload) {
        boost::json::object d;
        d["id"] = payload.requestId;
        d["status"] = statusObject(payload.statusCode);

        boost::json::object object;
        object["sid"] = responseSid(payload.meta);
        object["op"] = static_cast<std::uint8_t>(RpcOp::RequestBatchResponse);
        object["d"] = std::move(d);
        return boost::json::serialize(object);
    }

    static std::string serializeEvent(const RpcPayload& payload) {
        boost::json::object d;
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

        boost::json::object object;
        object["sid"] = responseSid(payload.meta);
        object["op"] = static_cast<std::uint8_t>(RpcOp::Event);
        object["d"] = std::move(d);
        return boost::json::serialize(object);
    }
};

}  // namespace axtp
