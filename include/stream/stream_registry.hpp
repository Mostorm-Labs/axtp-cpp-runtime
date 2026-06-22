#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "stream/stream_sink.hpp"

namespace axtp::stream {

inline std::string toHexU32(std::uint32_t value) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

class StreamRegistry {
public:
    explicit StreamRegistry(StreamCoreOptions options = {}, LogFn log = {})
        : _options(std::move(options)), _log(std::move(log)) {}

    static bool shouldLogChunkCount(std::uint64_t count) {
        return count <= 50 || (count % 100) == 0;
    }

    bool hasOpenStream(std::string_view kind, std::string_view source) const {
        std::lock_guard<std::mutex> lock(_mutex);
        for (const auto& entry : _streams) {
            const auto& info = entry.second.info;
            if (info.kind == kind && info.source == source) {
                return true;
            }
        }
        return false;
    }

    bool hasStream(std::uint32_t streamId) const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _streams.find(streamId) != _streams.end();
    }

    std::optional<StreamInfo> findStream(std::uint32_t streamId) const {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto it = _streams.find(streamId);
        if (it == _streams.end()) {
            return std::nullopt;
        }
        return it->second.info;
    }

    ErrorCode registerStream(StreamInfo info, StreamRegisterOptions options = {}) {
        if (info.streamId == 0) {
            return ErrorCode::StreamIdInvalid;
        }
        if (info.kind.empty()) {
            return ErrorCode::StreamPayloadInvalid;
        }

        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_streams.find(info.streamId) != _streams.end()) {
                return ErrorCode::StreamAlreadyOpen;
            }
            if (options.rejectDuplicateKindSource) {
                for (const auto& entry : _streams) {
                    const auto& existing = entry.second.info;
                    if (existing.kind == info.kind && existing.source == info.source) {
                        return ErrorCode::StreamAlreadyOpen;
                    }
                }
            }
        }

        StreamContext context;
        context.info = std::move(info);

        if (!_options.dumpDir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(_options.dumpDir, ec);
            auto prefix = options.dumpPrefix.empty() ? context.info.kind : options.dumpPrefix;
            auto extension = options.dumpExtension.empty() ? ".bin" : options.dumpExtension;
            if (!extension.empty() && extension.front() != '.') {
                extension.insert(extension.begin(), '.');
            }
            const auto path =
                _options.dumpDir / (prefix + "-" + toHexU32(context.info.streamId) + extension);
            context.dump.open(path, std::ios::binary);
            if (context.dump.is_open()) {
                logLine(context.info.kind + " dump=" + path.string());
            } else {
                logLine(context.info.kind + " dump open failed: " + path.string());
            }
        }

        const auto openedInfo = context.info;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _streams.emplace(context.info.streamId, std::move(context));
        }
        if (_options.streamSink != nullptr) {
            _options.streamSink->onStreamOpened(openedInfo);
        }
        return ErrorCode::Success;
    }

    bool close(std::uint32_t streamId, StreamInfo* closedInfo = nullptr) {
        std::optional<StreamInfo> info;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            const auto it = _streams.find(streamId);
            if (it == _streams.end()) {
                return false;
            }
            info = it->second.info;
            if (it->second.dump.is_open()) {
                it->second.dump.close();
            }
            _streams.erase(it);
        }

        if (closedInfo != nullptr) {
            *closedInfo = *info;
        }
        if (_options.streamSink != nullptr) {
            _options.streamSink->onStreamClosed(*info);
        }
        return true;
    }

    void handleStream(const StreamPayload& stream) {
        std::optional<StreamInfo> infoForSink;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto it = _streams.find(stream.streamId);
            if (it == _streams.end()) {
                ++_stats.unknownChunks;
                logLine("STREAM unknown streamId=" + toHexU32(stream.streamId) +
                        " seq=" + std::to_string(stream.seqId) +
                        " bytes=" + std::to_string(stream.data.size()));
                return;
            }

            auto& context = it->second;
            if (context.hasSeq) {
                if (stream.seqId == context.expectedSeq - 1U) {
                    ++_stats.duplicateSeq;
                } else if (stream.seqId != context.expectedSeq) {
                    ++_stats.seqGaps;
                    logLine(context.info.kind + " seq gap streamId=" +
                            toHexU32(stream.streamId) +
                            " expected=" + std::to_string(context.expectedSeq) +
                            " got=" + std::to_string(stream.seqId));
                }
            }
            context.hasSeq = true;
            context.expectedSeq = stream.seqId + 1U;
            context.lastCursor = stream.cursor;
            context.chunks += 1;
            context.bytes += stream.data.size();
            ++_stats.chunks;
            _stats.bytes += stream.data.size();

            if (context.dump.is_open() && !stream.data.empty()) {
                context.dump.write(reinterpret_cast<const char*>(stream.data.data()),
                                   static_cast<std::streamsize>(stream.data.size()));
            }

            if (shouldLogChunkCount(context.chunks)) {
                logLine(context.info.kind + " STREAM streamId=" + toHexU32(stream.streamId) +
                        " seq=" + std::to_string(stream.seqId) +
                        " chunkBytes=" + std::to_string(stream.data.size()) +
                        " chunks=" + std::to_string(context.chunks) +
                        " totalBytes=" + std::to_string(context.bytes) +
                        " cursor=" + std::to_string(stream.cursor));
            }
            infoForSink = context.info;
        }

        if (infoForSink.has_value() && _options.streamSink != nullptr) {
            _options.streamSink->onStreamChunk(*infoForSink, stream);
        }
    }

    StreamStats stats() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _stats;
    }

    std::size_t activeStreamCount() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _streams.size();
    }

    std::vector<ActiveStream> activeStreamsSnapshot() const {
        std::vector<ActiveStream> snapshot;
        std::lock_guard<std::mutex> lock(_mutex);
        snapshot.reserve(_streams.size());
        for (const auto& entry : _streams) {
            const auto& info = entry.second.info;
            snapshot.push_back(ActiveStream{
                info.streamId,
                info.kind,
                info.source,
                info.streamProfile,
            });
        }
        return snapshot;
    }

private:
    struct StreamContext {
        StreamInfo info;
        std::uint32_t expectedSeq = 0;
        bool hasSeq = false;
        std::uint64_t lastCursor = 0;
        std::uint64_t chunks = 0;
        std::uint64_t bytes = 0;
        std::ofstream dump;
    };

    void logLine(const std::string& line) const {
        if (_log) {
            _log(line);
        }
    }

    StreamCoreOptions _options;
    LogFn _log;
    mutable std::mutex _mutex;
    std::map<std::uint32_t, StreamContext> _streams;
    StreamStats _stats;
};

} // namespace axtp::stream
