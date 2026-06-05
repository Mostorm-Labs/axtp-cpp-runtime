#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "core/inbound/frame_decoder.hpp"
#include "model/message.hpp"

namespace axtp {

class IMessageSink {
public:
    virtual ~IMessageSink() = default;
    virtual void onMessage(Message message) = 0;
};

class MessageReassembler : public IFrameSink {
public:
    explicit MessageReassembler(IMessageSink& next, std::size_t maxMessageSize = 1024 * 1024)
        : _next(next)
        , _maxMessageSize(maxMessageSize) {}

    void onFrame(Frame frame) override {
        if (frame.header.frameCount == 1) {
            if (frame.header.frameIndex != 0) {
                return;
            }
            _next.onMessage(Message{
                frame.header.messageId, frame.header.payloadType, std::move(frame.payload)});
            return;
        }

        auto& assembly = _assemblies[frame.header.messageId];
        if (assembly.fragments.empty()) {
            assembly.payloadType = frame.header.payloadType;
            assembly.frameCount = frame.header.frameCount;
            assembly.fragments.resize(frame.header.frameCount);
        }

        if (assembly.payloadType != frame.header.payloadType ||
            assembly.frameCount != frame.header.frameCount ||
            frame.header.frameIndex >= assembly.fragments.size()) {
            _assemblies.erase(frame.header.messageId);
            return;
        }

        auto& slot = assembly.fragments[frame.header.frameIndex];
        if (slot.has_value()) {
            return;
        }
        assembly.totalSize += frame.payload.size();
        if (assembly.totalSize > _maxMessageSize) {
            _assemblies.erase(frame.header.messageId);
            return;
        }
        slot = std::move(frame.payload);

        for (const auto& fragment : assembly.fragments) {
            if (!fragment.has_value()) {
                return;
            }
        }

        Message message;
        message.messageId = frame.header.messageId;
        message.payloadType = assembly.payloadType;
        message.body.reserve(assembly.totalSize);
        for (auto& fragment : assembly.fragments) {
            message.body.insert(message.body.end(), fragment->begin(), fragment->end());
        }
        _assemblies.erase(message.messageId);
        _next.onMessage(std::move(message));
    }

private:
    struct Assembly {
        PayloadType payloadType = PayloadType::Rpc;
        std::uint8_t frameCount = 0;
        std::size_t totalSize = 0;
        std::vector<std::optional<Bytes>> fragments;
    };

    IMessageSink& _next;
    std::size_t _maxMessageSize;
    std::map<std::uint16_t, Assembly> _assemblies;
};

}  // namespace axtp
