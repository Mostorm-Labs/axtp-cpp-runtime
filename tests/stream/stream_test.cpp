#include "stream/stream_registry.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct RecordingSink final : axtp::stream::IStreamSink {
    std::vector<axtp::stream::StreamInfo> opened;
    std::vector<axtp::StreamPayload> chunks;
    std::vector<axtp::stream::StreamInfo> closed;

    void onStreamOpened(const axtp::stream::StreamInfo& info) override {
        opened.push_back(info);
    }

    void onStreamChunk(const axtp::stream::StreamInfo&, const axtp::StreamPayload& stream) override {
        chunks.push_back(stream);
    }

    void onStreamClosed(const axtp::stream::StreamInfo& info) override {
        closed.push_back(info);
    }
};

axtp::StreamPayload chunk(std::uint32_t streamId, std::uint32_t seq, std::uint64_t cursor, std::size_t bytes) {
    axtp::StreamPayload payload;
    payload.streamId = streamId;
    payload.seqId = seq;
    payload.cursor = cursor;
    payload.data.assign(bytes, static_cast<axtp::Byte>(seq + 1U));
    return payload;
}

std::uint64_t fileSize(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    return input.is_open() ? static_cast<std::uint64_t>(input.tellg()) : 0;
}

} // namespace

int main() {
    const auto dumpDir =
        std::filesystem::temp_directory_path() /
        ("axtp_stream_test_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(dumpDir);

    RecordingSink sink;
    axtp::stream::StreamRegistry registry(
        axtp::stream::StreamCoreOptions{dumpDir, &sink});

    axtp::stream::StreamInfo info;
    info.streamId = 0x10;
    info.kind = "file";
    info.source = "firmware.bin";
    info.streamProfile = "file.transfer";
    info.cursorUnit = "offsetBytes";
    info.payloadFormat = "binary";

    assert(registry.registerStream(
               info,
               axtp::stream::StreamRegisterOptions{"file", ".bin", true}) ==
           axtp::ErrorCode::Success);
    assert(sink.opened.size() == 1);
    assert(registry.hasStream(0x10));
    assert(registry.hasOpenStream("file", "firmware.bin"));
    assert(registry.activeStreamCount() == 1);

    assert(registry.registerStream(
               info,
               axtp::stream::StreamRegisterOptions{"file", ".bin", true}) ==
           axtp::ErrorCode::StreamAlreadyOpen);

    registry.handleStream(chunk(0x10, 0, 0, 3));
    registry.handleStream(chunk(0x10, 2, 3, 5));
    registry.handleStream(chunk(0x10, 2, 3, 7));
    registry.handleStream(chunk(0x99, 0, 0, 11));

    const auto stats = registry.stats();
    assert(stats.chunks == 3);
    assert(stats.bytes == 15);
    assert(stats.seqGaps == 1);
    assert(stats.duplicateSeq == 1);
    assert(stats.unknownChunks == 1);
    assert(sink.chunks.size() == 3);

    axtp::stream::StreamInfo closed;
    assert(registry.close(0x10, &closed));
    assert(closed.streamId == 0x10);
    assert(sink.closed.size() == 1);
    assert(registry.activeStreamCount() == 0);
    assert(!registry.close(0x10));

    const auto dumpPath = dumpDir / "file-0x00000010.bin";
    assert(fileSize(dumpPath) == 15);

    std::filesystem::remove_all(dumpDir);
    return 0;
}
