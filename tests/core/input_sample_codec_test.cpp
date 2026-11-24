#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>
#include "core/protocol/wire/input_sample_codec.hpp"
#include "core/protocol/wire/input_frame_codec.hpp"

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
axtp::Bytes hex(const std::string& text) {
    axtp::Bytes out;
    for (std::size_t i = 0; i < text.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
    return out;
}
}

int main(int argc, char** argv) {
    using namespace axtp::input;
    try {
        require(argc == 2, "expected exact generated vector file");
        std::ifstream file(argv[1]);
        const auto vectors = nlohmann::json::parse(file);
        std::size_t positives = 0, negatives = 0;
        for (const auto& trace : vectors.at("cases")) {
            InputFrameDecoder frames;
            InputState state;
            std::uint32_t seq = 0;
            bool rejected = false;
            for (const auto& row : trace.at("frames")) {
                std::optional<axtp::StreamPayload> packet;
                std::string error;
                if (!frames.consume(hex(row.at("hex").get<std::string>()), packet, &error) || !packet) {
                    rejected = true; break;
                }
                InputSample sample;
                InputState next;
                if (packet->seqId != seq + 1 || !decodeInputSample(packet->data, sample, {}, &error) ||
                    !validateTransition(sample, state, next, {}, &error)) {
                    rejected = true; break;
                }
                axtp::Bytes encoded;
                require(encodeInputSample(sample, encoded, {}, &error) && encoded == packet->data,
                        "cross-language sample does not roundtrip exactly");
                if (trace.at("valid").get<bool>()) {
                    const auto& expected = row.at("expectDecode");
                    require(packet->streamId == expected.at("streamId") &&
                            packet->seqId == expected.at("seqId") && packet->cursor == expected.at("cursor"),
                            "STREAM metadata differs");
                    require(packet->data.size() == expected.at("dataBytes"), "sample byte length differs");
                    if (sample.type == InputType::Touch) {
                        require(sample.contacts.size() == expected.at("records").size(), "finger count differs");
                        for (std::size_t i = 0; i < sample.contacts.size(); ++i) {
                            const auto& actual = sample.contacts[i]; const auto& want = expected.at("records")[i];
                            require(actual.contactId == want.at("contactId") && actual.phase == want.at("phase") &&
                                    actual.x == want.at("x") && actual.y == want.at("y"), "contact fields differ");
                        }
                    } else {
                        require(sample.mouse.eventKind == expected.at("eventKind"), "mouse kind differs");
                        if (expected.contains("wheelX"))
                            require(sample.mouse.wheelX == expected.at("wheelX") && sample.mouse.wheelY == expected.at("wheelY"), "signed wheel fields differ");
                    }
                    ++positives;
                }
                state = std::move(next); seq = packet->seqId;
            }
            require(rejected != trace.at("valid").get<bool>(), "unexpected trace result: " + trace.at("name").get<std::string>());
            if (!trace.at("valid").get<bool>()) ++negatives;
        }
        require(positives == 25 && negatives == 13, "missing formal vector coverage");

        InputSample multi;
        for (std::uint16_t id = 1; id <= 5; ++id) multi.contacts.push_back({id, 0, 10000, 20000});
        axtp::StreamPayload stream; stream.streamId = 7; stream.seqId = 1; stream.cursor = 1000;
        require(encodeInputSample(multi, stream.data), "five fingers encode");
        std::vector<axtp::Bytes> pieces;
        InputFrameOptions options; options.maxFrameBytes = 64;
        require(encodeInputFrames(stream, pieces, options) && pieces.size() == 2, "expected two complete Standard Frames");
        InputFrameDecoder decoder(64);
        std::optional<axtp::StreamPayload> result;
        require(decoder.consume(pieces[0], result) && !result, "fragment leaked a partial sample");
        require(decoder.consume(pieces[1], result) && result && result->data == stream.data, "fragment reassembly differs");
        decoder.reset();
        require(!decoder.consume(pieces[1], result), "orphan fragment accepted");
        auto bad = pieces[0]; bad.back() ^= 1;
        require(!decoder.consume(bad, result), "bad CRC accepted");
        InputSample single; single.contacts.push_back({7, 0, 10000, 20000});
        require(encodeInputSample(single, stream.data), "small frame sample");
        options.maxFrameBytes = 20;
        require(encodeInputFrames(stream, pieces, options) && pieces.size() == 4,
                "legal small frame budget rejected");
        InputFrameDecoder small(20);
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            require(small.consume(pieces[i], result), "small fragment rejected");
            require(bool(result) == (i + 1 == pieces.size()), "small sample completed at wrong boundary");
        }
        InputSample large;
        for (std::uint16_t id = 0; id < 256; ++id) large.contacts.push_back({id, 0, 0, 0});
        require(encodeInputSample(large, stream.data), "structural maximum sample");
        options.maxFrameBytes = 15;
        require(!encodeInputFrames(stream, pieces, options), "fragment count overflow accepted");
        std::cout << "25 cross-language frames, 13 rejected traces, fragmentation and CRC passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
