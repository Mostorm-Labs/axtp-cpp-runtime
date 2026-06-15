#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <atomic>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

#include "transport/transport.hpp"

namespace axtp {

struct HidTransportStats {
    std::uint64_t readReports = 0;
    std::uint64_t readBytes = 0;
    std::uint64_t acceptedReports = 0;
    std::uint64_t droppedReportId = 0;
    std::uint64_t readErrors = 0;
    std::uint64_t queuedReports = 0;
    std::uint64_t writeReports = 0;
    std::uint64_t writeBytes = 0;
    std::uint64_t writeErrors = 0;
};

struct HidTransportOptions {
    std::uint16_t vendorId = 0;
    std::uint16_t productId = 0;
    std::string serialNumber;
    std::uint8_t reportId = 0;
    std::size_t inputReportSize = 64;
    std::size_t outputReportSize = 64;
    std::size_t maxReportsPerPoll = 16;
    bool useReadThread = false;
    std::uint32_t readThreadSleepMs = 1;
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
    HidTransportStats stats() const;

private:
    std::unique_ptr<IHidBackend> makeDefaultBackend() const;
    std::size_t outputPayloadSize() const;
    void startReadThread();
    void stopReadThread();
    void readLoop();
    void handleReadReport(const Byte* data, std::size_t size, bool queueForPoll);
    void drainQueuedReports();

    HidTransportOptions _options;
    std::unique_ptr<IHidBackend> _backend;
    IByteSink* _sink = nullptr;
    std::atomic<bool> _open{false};
    std::atomic<bool> _readStop{false};
    std::thread _readThread;
    mutable std::mutex _rxMutex;
    std::queue<Bytes> _rxQueue;
    std::atomic<std::uint64_t> _readReports{0};
    std::atomic<std::uint64_t> _readBytes{0};
    std::atomic<std::uint64_t> _acceptedReports{0};
    std::atomic<std::uint64_t> _droppedReportId{0};
    std::atomic<std::uint64_t> _readErrors{0};
    std::atomic<std::uint64_t> _writeReports{0};
    std::atomic<std::uint64_t> _writeBytes{0};
    std::atomic<std::uint64_t> _writeErrors{0};
};

}  // namespace axtp
