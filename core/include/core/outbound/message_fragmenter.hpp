#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "model/frame.hpp"
#include "model/message.hpp"

namespace axtp {

class MessageFragmenter {
public:
    explicit MessageFragmenter(std::size_t maxFrameSize = 4096)
        : _maxFrameSize(maxFrameSize) {}

    void setMaxFrameSize(std::size_t maxFrameSize) {
        _maxFrameSize = maxFrameSize;
    }

    std::size_t maxFrameSize() const {
        return _maxFrameSize;
    }

    std::vector<Frame> fragment(Message message) {
        const std::size_t maxPayloadSize = payloadCapacity();
        if (maxPayloadSize == 0 || message.body.empty()) {
            return {makeFrame(message, 0, 1, Bytes{})};
        }

        const auto frameCount =
            static_cast<std::uint8_t>((message.body.size() + maxPayloadSize - 1) / maxPayloadSize);
        std::vector<Frame> frames;
        frames.reserve(frameCount);
        for (std::uint8_t index = 0; index < frameCount; ++index) {
            const std::size_t offset = static_cast<std::size_t>(index) * maxPayloadSize;
            const std::size_t chunkSize = std::min(maxPayloadSize, message.body.size() - offset);
            Bytes payload(message.body.begin() + static_cast<std::ptrdiff_t>(offset),
                          message.body.begin() + static_cast<std::ptrdiff_t>(offset + chunkSize));
            frames.push_back(makeFrame(message, index, frameCount, std::move(payload)));
        }
        return frames;
    }

private:
    std::size_t payloadCapacity() const {
        if (_maxFrameSize <= kStandardFrameHeaderSize + kStandardFrameCrcSize) {
            return 0;
        }
        return _maxFrameSize - kStandardFrameHeaderSize - kStandardFrameCrcSize;
    }

    std::uint16_t nextMessageId() {
        const auto id = _nextMessageId;
        ++_nextMessageId;
        if (_nextMessageId == 0) {
            _nextMessageId = 1;
        }
        return id;
    }

    Frame makeFrame(const Message& message,
                    std::uint8_t frameIndex,
                    std::uint8_t frameCount,
                    Bytes payload) {
        Frame frame;
        frame.header.version = kAxtpVersion1;
        frame.header.payloadType = message.payloadType;
        frame.header.payloadLength = static_cast<std::uint16_t>(payload.size());
        frame.header.messageId = _currentMessageId;
        if (frameIndex == 0) {
            _currentMessageId = nextMessageId();
            frame.header.messageId = _currentMessageId;
        }
        frame.header.frameIndex = frameIndex;
        frame.header.frameCount = frameCount;
        frame.payload = std::move(payload);
        return frame;
    }

    std::uint16_t _nextMessageId = 1;
    std::uint16_t _currentMessageId = 1;
    std::size_t _maxFrameSize;
};

}  // namespace axtp
