#pragma once

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <cstdint>
#include <memory>

#include "transport/transport.hpp"

namespace axtp {

class WebSocketTransport : public ITransport {
public:
    explicit WebSocketTransport(std::uint16_t port, const char* address = "127.0.0.1")
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
        if (_websocket) {
            _websocket->close(boost::beast::websocket::close_code::normal, ec);
            _websocket.reset();
        }
        _acceptor.close(ec);
        _open = false;
    }

    void poll() override {
        if (!_open) {
            return;
        }
        acceptOne();
        readOne();
    }

    void sendBytes(const Byte* data, std::size_t size) override {
        if (!_websocket) {
            return;
        }
        boost::system::error_code ec;
        _websocket->text(true);
        _websocket->write(boost::asio::buffer(data, size), ec);
    }

    TransportProfile profile() const override {
        TransportProfile profile;
        profile.kind = TransportKind::WebSocket;
        profile.wireMode = AxtpWireMode::WebSocketJsonRpc;
        profile.defaultRpcEncoding = RpcEncoding::Json;
        profile.messageOriented = true;
        profile.supportsTextMessage = true;
        profile.supportsBinaryMessage = false;
        return profile;
    }

    std::uint16_t localPort() const {
        boost::system::error_code ec;
        return _acceptor.is_open() ? _acceptor.local_endpoint(ec).port() : 0;
    }

    bool hasConnection() const {
        return _websocket != nullptr;
    }

private:
    using WebSocket = boost::beast::websocket::stream<boost::asio::ip::tcp::socket>;

    void acceptOne() {
        if (_websocket != nullptr) {
            return;
        }
        boost::asio::ip::tcp::socket socket(_io);
        boost::system::error_code ec;
        _acceptor.accept(socket, ec);
        if (ec) {
            return;
        }
        _websocket = std::make_unique<WebSocket>(std::move(socket));
        _websocket->accept(ec);
        if (ec) {
            _websocket.reset();
            return;
        }
    }

    void readOne() {
        if (!_websocket || !_sink) {
            return;
        }
        boost::system::error_code availableEc;
        if (_websocket->next_layer().available(availableEc) == 0 || availableEc) {
            return;
        }
        boost::beast::flat_buffer buffer;
        boost::system::error_code ec;
        _websocket->read(buffer, ec);
        if (ec) {
            return;
        }
        auto bytes = boost::beast::buffers_to_string(buffer.data());
        _sink->onBytes(reinterpret_cast<const Byte*>(bytes.data()), bytes.size());
    }

    boost::asio::io_context _io;
    boost::asio::ip::tcp::endpoint _endpoint;
    boost::asio::ip::tcp::acceptor _acceptor;
    std::unique_ptr<WebSocket> _websocket;
    IByteSink* _sink = nullptr;
    bool _open = false;
};

}  // namespace axtp
