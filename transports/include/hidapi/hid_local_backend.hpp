#pragma once

#include <memory>
#include <string>

#include "hidapi/hid_transport.hpp"

namespace axtp {

class LocalHidBackend final : public IHidBackend {
public:
    static std::unique_ptr<IHidBackend> client(std::string path);
    static std::unique_ptr<IHidBackend> server(std::string path);

    ~LocalHidBackend() override;

    bool open(const HidTransportOptions& options) override;
    void close() override;
    bool writeReport(const Byte* data, std::size_t size) override;
    std::optional<std::size_t> readReport(Byte* data, std::size_t size) override;

private:
    enum class Mode {
        Client,
        Server,
    };

    LocalHidBackend(Mode mode, std::string path);

    Mode _mode;
    std::string _path;
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace axtp
