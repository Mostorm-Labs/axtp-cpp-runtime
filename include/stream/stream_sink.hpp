#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "axtp_core.hpp"

namespace axtp::stream {

using LogFn = std::function<void(std::string_view)>;

struct StreamInfo {
    std::uint32_t streamId = 0;
    std::string kind;
    std::string source;
    std::string streamProfile;
    std::string cursorUnit;
    std::string payloadFormat;
    nlohmann::json metadata = nlohmann::json::object();
};

struct ActiveStream {
    std::uint32_t streamId = 0;
    std::string kind;
    std::string source;
    std::string streamProfile;
};

struct StreamStats {
    std::uint64_t chunks = 0;
    std::uint64_t bytes = 0;
    std::uint64_t unknownChunks = 0;
    std::uint64_t seqGaps = 0;
    std::uint64_t duplicateSeq = 0;
};

struct StreamRegisterOptions {
    std::string dumpPrefix;
    std::string dumpExtension;
    bool rejectDuplicateKindSource = true;
};

class IStreamSink {
public:
    virtual ~IStreamSink() = default;
    virtual void onStreamOpened(const StreamInfo& info) = 0;
    virtual void onStreamChunk(const StreamInfo& info, const StreamPayload& stream) = 0;
    virtual void onStreamClosed(const StreamInfo& info) = 0;
};

struct StreamCoreOptions {
    std::filesystem::path dumpDir;
    IStreamSink* streamSink = nullptr;
};

} // namespace axtp::stream
