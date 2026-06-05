#pragma once

#include <array>
#include <boost/asio.hpp>
#include <cstdint>
#include <memory>

#include "transport/transport.hpp"

namespace axtp {

class TcpTransport : public ITransport {
public:
    explicit TcpTransport(std::uint16_t port, const char* address = "127.0.0.1")
        : _endpoint(boost::asio::ip::make_address(address), port)
        , _acceptor(_io) {}

    void bind(IByteSink& sink) override {
        _sink = &sink;
    }

    void open() override {
        boost::system::error_code ec;
        _acceptor.open(_endpoint.protocol(), ec);
        _acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true), ec);
        _acceptor.bind(_endpoint, ec);
        _acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
        _acceptor.non_blocking(true, ec);
        _open = !ec;
    }

    void close() override {
        boost::system::error_code ec;
        if (_socket) {
            _socket->close(ec);
            _socket.reset();
        }
        _acceptor.close(ec);
        _open = false;
    }

    void poll() override {
        if (!_open) {
            return;
        }
        acceptOne();
        readAvailable();
    }

    void sendBytes(const Byte* data, std::size_t size) override {
        if (!_socket || !_socket->is_open()) {
            return;
        }
        boost::system::error_code ec;
        boost::asio::write(*_socket, boost::asio::buffer(data, size), ec);
    }

    TransportProfile profile() const override {
        TransportProfile profile;
        profile.kind = TransportKind::Tcp;
        profile.wireMode = AxtpWireMode::FramedBinary;
        profile.defaultRpcEncoding = RpcEncoding::Tlv;
        profile.messageOriented = false;
        profile.supportsTextMessage = false;
        profile.supportsBinaryMessage = true;
        return profile;
    }

    std::uint16_t localPort() const {
        boost::system::error_code ec;
        return _acceptor.is_open() ? _acceptor.local_endpoint(ec).port() : 0;
    }

private:
    void acceptOne() {
        if (_socket && _socket->is_open()) {
            return;
        }
        auto socket = std::make_unique<boost::asio::ip::tcp::socket>(_io);
        boost::system::error_code ec;
        _acceptor.accept(*socket, ec);
        if (ec) {
            return;
        }
        socket->non_blocking(true, ec);
        _socket = std::move(socket);
    }

    void readAvailable() {
        if (!_socket || !_sink) {
            return;
        }
        std::array<Byte, 4096> buffer{};
        while (true) {
            boost::system::error_code ec;
            const auto n = _socket->read_some(boost::asio::buffer(buffer), ec);
            if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) {
                return;
            }
            if (ec || n == 0) {
                close();
                return;
            }
            _sink->onBytes(buffer.data(), n);
        }
    }

    boost::asio::io_context _io;
    boost::asio::ip::tcp::endpoint _endpoint;
    boost::asio::ip::tcp::acceptor _acceptor;
    std::unique_ptr<boost::asio::ip::tcp::socket> _socket;
    IByteSink* _sink = nullptr;
    bool _open = false;
};

}  // namespace axtp
