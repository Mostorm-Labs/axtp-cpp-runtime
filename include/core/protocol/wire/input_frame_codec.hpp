#pragma once

#include <optional>
#include <utility>
#include <vector>

#include "core/protocol/wire/input_sample_codec.hpp"
#include "core/protocol/wire/framed_binary/inbound/frame_decoder.hpp"
#include "core/protocol/wire/framed_binary/outbound/frame_encoder.hpp"
#include "core/protocol/wire/framed_binary/outbound/message_fragmenter.hpp"
#include "core/protocol/wire/framed_binary/outbound/payload_encoder.hpp"

namespace axtp::input {

struct InputFrameOptions {
    std::size_t maxFrameBytes = 4096;
    std::uint8_t sourceId = 0;
    std::uint8_t destinationId = 0;
    std::uint16_t messageId = 1;
};

inline bool encodeInputFrames(const StreamPayload& stream, std::vector<Bytes>& output,
                              const InputFrameOptions& options = {}, std::string* error = nullptr) {
    InputSample checked;
    if (!stream.streamId || !stream.seqId || options.maxFrameBytes <= 14 || options.maxFrameBytes > 65549)
        return detail::fail(error, "input stream/frame limits");
    if (!decodeInputSample(stream.data, checked, {}, error)) return false;
    const auto capacity = options.maxFrameBytes - kStandardFrameHeaderSize - kStandardFrameCrcSize;
    const auto payloadBytes = input_wire::kStreamHeaderBytes + stream.data.size();
    if ((payloadBytes + capacity - 1) / capacity > 255)
        return detail::fail(error, "input fragment count limit");
    PayloadEncoder payloadEncoder;
    MessageFragmenter fragmenter(options.maxFrameBytes);
    FrameEncoder encoder;
    auto frames = fragmenter.fragment(payloadEncoder.encodeStream(stream));
    std::vector<Bytes> candidate;
    for (auto& frame : frames) {
        frame.header.sourceId = options.sourceId;
        frame.header.destinationId = options.destinationId;
        frame.header.messageId = options.messageId;
        candidate.push_back(encoder.encode(frame));
    }
    output = std::move(candidate);
    if (error) error->clear();
    return true;
}

// One bounded, ordered STREAM message assembly per authenticated caller context.
// consume expects one complete Standard Frame; transport/session dispatch owns
// demultiplexing CONTROL/RPC and binding the caller. This class never injects.
class InputFrameDecoder {
public:
    explicit InputFrameDecoder(std::size_t maxFrameBytes = 4096) : _maxFrameBytes(maxFrameBytes) {}

    void reset() { _body.clear(); _next = 0; _count = 0; }

    bool consume(const Bytes& bytes, std::optional<StreamPayload>& output,
                 std::string* error = nullptr) {
        output.reset();
        if (error) error->clear();
        const auto reject = [&](const char* text) { reset(); return detail::fail(error, text); };
        if (bytes.size() < kStandardFrameHeaderSize + kStandardFrameCrcSize ||
            bytes.size() > _maxFrameBytes) return reject("input frame length/budget");
        const auto size = (std::size_t(bytes[4]) << 8) | bytes[5];
        if (bytes.size() != size + kStandardFrameHeaderSize + kStandardFrameCrcSize)
            return reject("input frame exact length");
        Capture capture;
        FrameDecoder decoder(capture);
        decoder.onBytes(bytes.data(), bytes.size());
        if (!capture.frame || capture.frame->header.payloadType != PayloadType::Stream)
            return reject("input frame CRC/header/type");
        auto& f = *capture.frame;
        if (f.header.frameIndex == 0) {
            if (_next) return reject("overlapping input fragment assembly");
            _header = f.header; _count = f.header.frameCount;
        } else if (!_next || f.header.frameIndex != _next || f.header.frameCount != _count ||
                   f.header.messageId != _header.messageId || f.header.sourceId != _header.sourceId ||
                   f.header.destinationId != _header.destinationId) {
            return reject("orphan/duplicate input fragment");
        }
        if (_body.size() + f.payload.size() > input_wire::kStreamHeaderBytes + input_wire::kMaxDataBytes)
            return reject("input fragment assembly budget");
        _body.insert(_body.end(), f.payload.begin(), f.payload.end());
        ++_next;
        if (_next < _count) return true;
        if (_body.size() < input_wire::kStreamHeaderBytes + 2) return reject("input STREAM length");
        ByteReader reader(_body);
        StreamPayload stream;
        stream.streamId = reader.readU32().value;
        stream.seqId = reader.readU32().value;
        stream.cursor = reader.readU64().value;
        stream.data = reader.readBytes(reader.remaining()).value;
        if (!stream.streamId || !stream.seqId) return reject("input STREAM zero identity");
        reset();
        output = std::move(stream);
        if (error) error->clear();
        return true;
    }

private:
    struct Capture : IFrameSink {
        std::optional<Frame> frame;
        void onFrame(Frame value) override { frame = std::move(value); }
    };
    std::size_t _maxFrameBytes;
    Bytes _body;
    FrameHeader _header;
    std::uint16_t _next = 0;
    std::uint16_t _count = 0;
};
} // namespace axtp::input
