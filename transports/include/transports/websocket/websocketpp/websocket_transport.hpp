#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include "runtime/transport/transport.hpp"

namespace axtp {

class WebSocketppTransport : public ITransport {
public:
    explicit WebSocketppTransport(std::uint16_t port, const char* address = "127.0.0.1")
        : _port(port)
        , _address(address != nullptr ? address : "127.0.0.1") {}

    ~WebSocketppTransport() override {
        close();
    }

    void bind(IByteSink& sink) override {
        _sink = &sink;
    }

    void open() override {
        close();
        _server = std::make_unique<Server>();
        _server->clear_access_channels(websocketpp::log::alevel::all);
        _server->clear_error_channels(websocketpp::log::elevel::all);
        _server->init_asio();
        _server->set_reuse_addr(true);
        _server->set_open_handler([this](ConnectionHandle connection) {
            std::lock_guard<std::mutex> lock(_mutex);
            _connections.insert(connection);
        });
        _server->set_close_handler([this](ConnectionHandle connection) {
            std::lock_guard<std::mutex> lock(_mutex);
            _connections.erase(connection);
        });
        _server->set_message_handler(
            [this](ConnectionHandle, Server::message_ptr message) {
                if (!message || message->get_opcode() != websocketpp::frame::opcode::text) {
                    return;
                }
                std::lock_guard<std::mutex> lock(_mutex);
                _rxMessages.push(message->get_payload());
            });

        websocketpp::lib::error_code ec;
        _server->listen(_address, std::to_string(_port), ec);
        if (ec) {
            _server.reset();
            return;
        }
        _localPort = static_cast<std::uint16_t>(_server->get_local_endpoint(ec).port());
        _server->start_accept(ec);
        if (ec) {
            _server.reset();
            _localPort = 0;
            return;
        }
        _open = true;
        _thread = std::thread([this]() { _server->run(); });
    }

    void close() override {
        _open = false;
        if (_server) {
            websocketpp::lib::error_code ec;
            _server->stop_listening(ec);
            std::set<ConnectionHandle, std::owner_less<ConnectionHandle>> connections;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                connections = _connections;
                _connections.clear();
            }
            for (const auto& connection : connections) {
                _server->close(connection, websocketpp::close::status::normal, "", ec);
            }
            _server->stop();
        }
        if (_thread.joinable()) {
            _thread.join();
        }
        _server.reset();
        _localPort = 0;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            std::queue<std::string> empty;
            _rxMessages.swap(empty);
        }
    }

    void poll() override {
        if (!_open || _sink == nullptr) {
            return;
        }
        while (true) {
            std::string message;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                if (_rxMessages.empty()) {
                    return;
                }
                message = std::move(_rxMessages.front());
                _rxMessages.pop();
            }
            _sink->onBytes(reinterpret_cast<const Byte*>(message.data()), message.size());
        }
    }

    void sendBytes(const Byte* data, std::size_t size) override {
        if (!_server || data == nullptr || size == 0) {
            return;
        }
        const std::string text(reinterpret_cast<const char*>(data), size);
        std::set<ConnectionHandle, std::owner_less<ConnectionHandle>> connections;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            connections = _connections;
        }
        websocketpp::lib::error_code ec;
        for (const auto& connection : connections) {
            _server->send(connection, text, websocketpp::frame::opcode::text, ec);
        }
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
        return _localPort;
    }

    bool hasConnection() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _open && !_connections.empty();
    }

private:
    using Server = websocketpp::server<websocketpp::config::asio>;
    using ConnectionHandle = websocketpp::connection_hdl;

    std::uint16_t _port = 0;
    std::uint16_t _localPort = 0;
    std::string _address;
    std::unique_ptr<Server> _server;
    std::thread _thread;
    IByteSink* _sink = nullptr;
    bool _open = false;
    mutable std::mutex _mutex;
    std::set<ConnectionHandle, std::owner_less<ConnectionHandle>> _connections;
    std::queue<std::string> _rxMessages;
};

}  // namespace axtp
