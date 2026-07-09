#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "axtp_sdk.hpp"

namespace axtp::firmware {

struct FirmwareUpdateFile {
    std::string fileId = "firmware";
    std::string target;
    Bytes data;
    std::string md5;
};

struct FirmwareUpdateRequest {
    FirmwareUpdateFile file;
    std::string packageId;
    std::string version;
    std::uint32_t preferredChunkSize = 1024;
    std::string jsonSid;
};

struct FirmwareUpdateResult {
    bool ok = false;
    ErrorCode status = ErrorCode::Success;
    std::string failedMethod;
    std::string updateSessionId;
    std::uint32_t streamId = 0;
    std::uint32_t chunkSize = 0;
    std::uint32_t chunks = 0;
    std::uint64_t bytes = 0;
    nlohmann::json manifest = nlohmann::json::object();
    nlohmann::json begin = nlohmann::json::object();
    nlohmann::json finish = nlohmann::json::object();
};

namespace detail {

inline nlohmann::json parseJsonValueOrString(const Bytes& bytes) {
    if (bytes.empty()) {
        return nullptr;
    }
    try {
        return nlohmann::json::parse(std::string(bytes.begin(), bytes.end()));
    } catch (const std::exception&) {
        return std::string(bytes.begin(), bytes.end());
    }
}

inline std::optional<std::string> jsonString(const nlohmann::json& object, const char* field) {
    if (!object.is_object() || !object.contains(field) || !object[field].is_string()) {
        return std::nullopt;
    }
    return object[field].get<std::string>();
}

inline std::optional<std::uint32_t> jsonUint32(const nlohmann::json& object, const char* field) {
    if (!object.is_object() || !object.contains(field)) {
        return std::nullopt;
    }
    const auto& value = object[field];
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number <= std::numeric_limits<std::uint32_t>::max()) {
            return static_cast<std::uint32_t>(number);
        }
        return std::nullopt;
    }
    if (!value.is_number_integer()) {
        return std::nullopt;
    }
    const auto number = value.get<std::int64_t>();
    if (number < 0 ||
        static_cast<std::uint64_t>(number) > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(number);
}

inline RpcPayload makeJsonRequest(std::uint16_t methodId,
                                  std::string methodName,
                                  const nlohmann::json& params,
                                  const std::string& jsonSid) {
    const auto body = params.dump();
    RpcPayload request;
    request.encoding = RpcEncoding::Json;
    request.op = RpcOp::Request;
    request.methodOrEventId = methodId;
    request.bodyEncoding = RpcBodyEncoding::None;
    request.meta.sourceProtocol = SourceProtocol::JsonRpc;
    request.meta.jsonMethodOrEventName = std::move(methodName);
    request.meta.jsonSid = jsonSid;
    request.body.assign(body.begin(), body.end());
    return request;
}

inline nlohmann::json buildManifest(const FirmwareUpdateRequest& request) {
    auto file = nlohmann::json::object();
    file["fileId"] = request.file.fileId;
    if (!request.file.target.empty()) {
        file["target"] = request.file.target;
    }
    file["size"] = request.file.data.size();
    file["md5"] = request.file.md5;

    auto manifest = nlohmann::json::object();
    if (!request.packageId.empty()) {
        manifest["packageId"] = request.packageId;
    }
    if (!request.version.empty()) {
        manifest["version"] = request.version;
    }
    manifest["files"] = nlohmann::json::array({file});
    return manifest;
}

} // namespace detail

class FirmwareUpdateProfile {
public:
    explicit FirmwareUpdateProfile(sdk::AxtpClient& client)
        : client_(client) {}

    FirmwareUpdateResult update(const FirmwareUpdateRequest& request,
                                sdk::CallOptions options = {}) {
        FirmwareUpdateResult result;
        result.manifest = detail::buildManifest(request);
        result.bytes = request.file.data.size();
        result.chunkSize = request.preferredChunkSize == 0 ? 1024 : request.preferredChunkSize;

        options.encoding = RpcEncoding::Json;
        const auto beginMethodId = static_cast<std::uint16_t>(MethodId::FirmwareBeginUpdate);
        auto beginParams = nlohmann::json::object();
        beginParams["manifest"] = result.manifest;
        auto beginResponse = client_.callRaw(
            detail::makeJsonRequest(
                beginMethodId, "firmware.beginUpdate", beginParams, request.jsonSid),
            options);
        if (beginResponse.statusCode != ErrorCode::Success) {
            result.status = beginResponse.statusCode;
            result.failedMethod = "firmware.beginUpdate";
            result.begin = detail::parseJsonValueOrString(beginResponse.body);
            return result;
        }

        result.begin = detail::parseJsonValueOrString(beginResponse.body);
        const auto updateSessionId = detail::jsonString(result.begin, "updateSessionId");
        if (!updateSessionId.has_value() || !result.begin.is_object() ||
            !result.begin.contains("streams") || !result.begin["streams"].is_array()) {
            result.status = ErrorCode::RpcPayloadInvalid;
            result.failedMethod = "firmware.beginUpdate";
            return result;
        }
        result.updateSessionId = *updateSessionId;

        auto streamId = std::optional<std::uint32_t>{};
        for (const auto& binding : result.begin["streams"]) {
            const auto fileId = detail::jsonString(binding, "fileId");
            if (fileId.has_value() && *fileId == request.file.fileId) {
                streamId = detail::jsonUint32(binding, "streamId");
                break;
            }
        }
        if (!streamId.has_value()) {
            result.status = ErrorCode::RpcPayloadInvalid;
            result.failedMethod = "firmware.beginUpdate";
            return result;
        }
        result.streamId = *streamId;
        if (const auto recommendedChunkSize = detail::jsonUint32(result.begin, "chunkSize")) {
            if (*recommendedChunkSize > 0) {
                result.chunkSize = *recommendedChunkSize;
            }
        }

        for (std::size_t offset = 0; offset < request.file.data.size();
             offset += result.chunkSize) {
            const auto count =
                std::min<std::size_t>(result.chunkSize, request.file.data.size() - offset);
            StreamPayload stream;
            stream.streamId = result.streamId;
            stream.seqId = result.chunks;
            stream.cursor = offset;
            stream.data.assign(
                request.file.data.begin() + static_cast<std::ptrdiff_t>(offset),
                request.file.data.begin() + static_cast<std::ptrdiff_t>(offset + count));
            client_.sendStream(std::move(stream));
            ++result.chunks;
        }

        const auto finishMethodId = static_cast<std::uint16_t>(MethodId::FirmwareFinishUpdate);
        auto finishParams = nlohmann::json::object();
        finishParams["updateSessionId"] = result.updateSessionId;
        auto finishResponse = client_.callRaw(
            detail::makeJsonRequest(
                finishMethodId, "firmware.finishUpdate", finishParams, request.jsonSid),
            options);
        result.finish = detail::parseJsonValueOrString(finishResponse.body);
        if (finishResponse.statusCode != ErrorCode::Success) {
            result.status = finishResponse.statusCode;
            result.failedMethod = "firmware.finishUpdate";
            return result;
        }

        result.status = ErrorCode::Success;
        result.ok = result.finish.is_object() && result.finish.value("accepted", false);
        if (!result.ok) {
            result.failedMethod = "firmware.finishUpdate";
        }
        return result;
    }

private:
    sdk::AxtpClient& client_;
};

} // namespace axtp::firmware
