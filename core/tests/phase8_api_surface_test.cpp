#include <cassert>
#include <string>

#include "axtp.hpp"

namespace {

struct CapturingTextWriter : axtp::ITextWriter {
    std::string text;

    void writeText(const char* data, std::size_t size) override {
        text.append(data, size);
    }
};

struct CapturingTransportSink : axtp::ITransportSink {
    axtp::TransportPacket last;

    void onTransportPacket(const axtp::TransportPacket& packet) override {
        last = packet;
    }
};

}  // namespace

int main() {
    CapturingTextWriter writer;
    writer.writeText("ok", 2);
    assert(writer.text == "ok");

    const axtp::Byte bytes[] = {0x41, 0x58};
    axtp::TransportPacket packet;
    packet.kind = axtp::TransportPacketKind::TextMessage;
    packet.data = bytes;
    packet.size = 2;

    CapturingTransportSink sink;
    sink.onTransportPacket(packet);
    assert(sink.last.kind == axtp::TransportPacketKind::TextMessage);
    assert(sink.last.size == 2);
    assert(sink.last.data[0] == 0x41);

    auto registry = axtp::MethodRegistry::fromGeneratedDefaults();
    const auto methodId = registry.findMethodId("audio.getAlgorithmConfig");
    assert(methodId.has_value());
    assert(*methodId == 0x0901);
    registry.addMethod(0x90010001, "vendor.echo");
    assert(registry.containsMethod("vendor.echo"));

    axtp::BasicBroker broker;
    broker.registerMethod(0x90010001, [](const axtp::RpcPayload& request) { return request.body; });
    assert(broker.queuedTaskCount() == 0);
}
