#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "transport/transport.hpp"

namespace axtp {

struct HidTransportOptions {
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;
    std::string serialNumber;
    std::uint8_t reportId = 0;
    std::size_t inputReportSize = 64;
    std::size_t outputReportSize = 64;
    std::size_t maxReportsPerPoll = 16;
};

class IHidBackend {
public:
    virtual ~IHidBackend() = default;
    virtual bool open(const HidTransportOptions& options) = 0;
    virtual void close() = 0;
    virtual bool writeReport(const Byte* data, std::size_t size) = 0;
    virtual std::optional<std::size_t> readReport(Byte* data, std::size_t size) = 0;
};

class HidTransport : public ITransport {
public:
    explicit HidTransport(HidTransportOptions options = {});
    HidTransport(HidTransportOptions options, std::unique_ptr<IHidBackend> backend);
    ~HidTransport() override;

    void bind(IByteSink& sink) override;
    void open() override;
    void close() override;
    void poll() override;
    void sendBytes(const Byte* data, std::size_t size) override;
    TransportProfile profile() const override;

    bool isOpen() const;
    const HidTransportOptions& options() const;

private:
    std::unique_ptr<IHidBackend> makeDefaultBackend() const;
    std::size_t outputPayloadSize() const;

    HidTransportOptions _options;
    std::unique_ptr<IHidBackend> _backend;
    IByteSink* _sink = nullptr;
    bool _open = false;
};

}  // namespace axtp
