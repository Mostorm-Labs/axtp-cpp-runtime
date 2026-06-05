#include <cassert>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

#include "hidapi/hid_local_backend.hpp"

#if !defined(_WIN32)
#    include <unistd.h>
#endif

namespace {

std::optional<std::size_t>
readWithTimeout(axtp::IHidBackend& backend, axtp::Byte* data, std::size_t size) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto read = backend.readReport(data, size);
        if (!read.has_value() || *read == size) {
            return read;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 0;
}

}  // namespace

int main() {
#if defined(_WIN32)
    return 0;
#else
    const std::string path =
        std::string("/tmp/axtp-hid-backend-test-") + std::to_string(getpid()) + ".sock";

    axtp::HidTransportOptions options;
    options.inputReportSize = 8;
    options.outputReportSize = 8;

    auto server = axtp::LocalHidBackend::server(path);
    auto client = axtp::LocalHidBackend::client(path);
    assert(server->open(options));
    assert(client->open(options));

    const axtp::Byte request[] = {0x00, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7};
    assert(client->writeReport(request, sizeof(request)));

    axtp::Byte serverRead[8] = {};
    const auto serverReadSize = readWithTimeout(*server, serverRead, sizeof(serverRead));
    assert(serverReadSize.has_value());
    assert(*serverReadSize == sizeof(serverRead));
    for (std::size_t index = 0; index < sizeof(request); ++index) {
        assert(serverRead[index] == request[index]);
    }

    const axtp::Byte response[] = {0x00, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7};
    assert(server->writeReport(response, sizeof(response)));

    axtp::Byte clientRead[8] = {};
    const auto clientReadSize = readWithTimeout(*client, clientRead, sizeof(clientRead));
    assert(clientReadSize.has_value());
    assert(*clientReadSize == sizeof(clientRead));
    for (std::size_t index = 0; index < sizeof(response); ++index) {
        assert(clientRead[index] == response[index]);
    }

    client->close();
    server->close();
    assert(!server->writeReport(response, sizeof(response)));
    return 0;
#endif
}
