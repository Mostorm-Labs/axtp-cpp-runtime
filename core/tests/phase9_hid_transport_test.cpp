#include <algorithm>
#include <cassert>
#include <cstddef>
#include <optional>
#include <queue>
#include <utility>
#include <vector>

#include "broker/basic_broker.hpp"
#include "core/axtp_core.hpp"
#include "runtime/axtp_endpoint.hpp"
#include "hidapi/hid_transport.hpp"

namespace {

class MockHidBackend : public axtp::IHidBackend {
public:
    bool open(const axtp::HidTransportOptions&) override {
        _open = true;
        ++openCount;
        return true;
    }

    void close() override {
        _open = false;
        ++closeCount;
    }

    bool writeReport(const axtp::Byte* data, std::size_t size) override {
        if (!_open) {
            return false;
        }
        writes.emplace_back(data, data + size);
        return true;
    }

    std::optional<std::size_t> readReport(axtp::Byte* data, std::size_t size) override {
        if (!_open) {
            return std::nullopt;
        }
        if (reads.empty()) {
            return 0;
        }
        auto report = std::move(reads.front());
        reads.pop();
        const auto copied = std::min(size, report.size());
        std::copy(report.begin(), report.begin() + static_cast<std::ptrdiff_t>(copied), data);
        return copied;
    }

    void enqueueRead(axtp::Bytes report) {
        reads.push(std::move(report));
    }

    bool _open = false;
    int openCount = 0;
    int closeCount = 0;
    std::vector<axtp::Bytes> writes;
    std::queue<axtp::Bytes> reads;
};

class CapturingByteSink : public axtp::IByteSink {
public:
    void onBytes(const axtp::Byte* data, std::size_t size) override {
        chunks.emplace_back(data, data + size);
    }

    std::vector<axtp::Bytes> chunks;
};

}  // namespace

int main() {
    axtp::HidTransportOptions options;
    options.reportId = 0x05;
    options.inputReportSize = 5;
    options.outputReportSize = 5;
    options.maxReportsPerPoll = 8;

    auto backend = std::make_unique<MockHidBackend>();
    auto* backendPtr = backend.get();
    axtp::HidTransport transport(options, std::move(backend));

    const auto profile = transport.profile();
    assert(profile.kind == axtp::TransportKind::Hid);
    assert(profile.wireMode == axtp::AxtpWireMode::FramedBinary);
    assert(profile.messageOriented);
    assert(profile.supportsBinaryMessage);
    assert(!profile.supportsTextMessage);
    assert(profile.preferredFrameSize == 4);

    const axtp::Bytes beforeOpen{0x01, 0x02};
    transport.sendBytes(beforeOpen.data(), beforeOpen.size());
    assert(backendPtr->writes.empty());

    CapturingByteSink sink;
    transport.bind(sink);
    transport.open();
    assert(transport.isOpen());
    assert(backendPtr->openCount == 1);

    const axtp::Bytes payload{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A};
    transport.sendBytes(payload.data(), payload.size());
    assert(backendPtr->writes.size() == 3);
    assert((backendPtr->writes[0] == axtp::Bytes{0x05, 0x01, 0x02, 0x03, 0x04}));
    assert((backendPtr->writes[1] == axtp::Bytes{0x05, 0x05, 0x06, 0x07, 0x08}));
    assert((backendPtr->writes[2] == axtp::Bytes{0x05, 0x09, 0x0A, 0x00, 0x00}));

    backendPtr->writes.clear();
    const axtp::Bytes exact{0xA0, 0xA1, 0xA2, 0xA3, 0xB0, 0xB1, 0xB2, 0xB3};
    transport.sendBytes(exact.data(), exact.size());
    assert(backendPtr->writes.size() == 2);
    assert((backendPtr->writes[0] == axtp::Bytes{0x05, 0xA0, 0xA1, 0xA2, 0xA3}));
    assert((backendPtr->writes[1] == axtp::Bytes{0x05, 0xB0, 0xB1, 0xB2, 0xB3}));

    backendPtr->enqueueRead(axtp::Bytes{0x05, 0xC0, 0xC1, 0x00, 0x00});
    backendPtr->enqueueRead(axtp::Bytes{0x07, 0xDE, 0xAD, 0xBE, 0xEF});
    backendPtr->enqueueRead(axtp::Bytes{0x05, 0xD0, 0xD1, 0xD2, 0xD3});
    transport.poll();
    assert(sink.chunks.size() == 2);
    assert((sink.chunks[0] == axtp::Bytes{0xC0, 0xC1, 0x00, 0x00}));
    assert((sink.chunks[1] == axtp::Bytes{0xD0, 0xD1, 0xD2, 0xD3}));

    backendPtr->writes.clear();
    sink.chunks.clear();
    transport.close();
    assert(!transport.isOpen());
    assert(backendPtr->closeCount >= 1);
    backendPtr->enqueueRead(axtp::Bytes{0x05, 0xE0, 0xE1, 0xE2, 0xE3});
    transport.sendBytes(payload.data(), payload.size());
    transport.poll();
    assert(backendPtr->writes.empty());
    assert(sink.chunks.empty());

    auto coreBackend = std::make_unique<MockHidBackend>();
    auto* coreBackendPtr = coreBackend.get();
    axtp::HidTransport coreTransport(options, std::move(coreBackend));
    coreTransport.open();
    axtp::BasicBroker<> coreBroker;
    axtp::AxtpEndpoint coreEndpoint(coreBroker);
    coreEndpoint.attachTransport(coreTransport);
    axtp::RpcPayload response;
    response.encoding = axtp::RpcEncoding::Tlv;
    response.op = axtp::RpcOp::RequestResponse;
    response.requestId = 0x01020304;
    response.methodOrEventId = 0x0101;
    response.bodyEncoding = axtp::RpcBodyEncoding::Tlv8;
    response.body = {0x10, 0x11, 0x12, 0x13, 0x14};
    coreEndpoint.core().handleBrokerResult(axtp::BrokerResult::rpcResponse(std::move(response)));
    coreEndpoint.flushOutbound();
    assert(!coreBackendPtr->writes.empty());
    for (const auto& report : coreBackendPtr->writes) {
        assert(report.size() == options.outputReportSize);
        assert(report[0] == options.reportId);
    }
    assert(coreBackendPtr->writes[0][1] == axtp::kAxtpStandardMagic0);
    assert(coreBackendPtr->writes[0][2] == axtp::kAxtpStandardMagic1);

    return 0;
}
