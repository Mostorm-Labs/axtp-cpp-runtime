#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <string>

#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketMessage.h>
#include <ixwebsocket/IXWebSocketMessageType.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include "core/runtime/transport/transport.hpp"

namespace axtp {

class WebSocketTransport : public ITransport {
public:
    explicit WebSocketTransport(std::uint16_t port, const char* address = "127.0.0.1")
        : _port(port)
        , _address(address != nullptr ? address : "127.0.0.1") {}

    ~WebSocketTransport() override {
        close();
    }

    void bind(IByteSink& sink) override {
        _sink = &sink;
    }

    void open() override {
        close();
        _netInitialized = ix::initNetSystem();
        const auto listenPort = _port == 0 ? ix::getFreePort() : static_cast<int>(_port);
        if (listenPort <= 0) {
            return;
        }
        _localPort = static_cast<std::uint16_t>(listenPort);
        _server = std::make_unique<ix::WebSocketServer>(listenPort, _address);
        _server->disablePerMessageDeflate();
        _server->setOnClientMessageCallback(
            [this](std::shared_ptr<ix::ConnectionState> connectionState,
                   ix::WebSocket&,
                   const ix::WebSocketMessagePtr& message) {
                if (!message) {
                    return;
                }
                if (message->type == ix::WebSocketMessageType::Open) {
                    {
                        std::lock_guard<std::mutex> lock(_clientsMutex);
                        _activeClients.insert(connectionId(connectionState));
                    }
                    _connectionGeneration.fetch_add(1, std::memory_order_relaxed);
                    _hasConnection.store(true);
                    return;
                }
                if (message->type == ix::WebSocketMessageType::Close ||
                    message->type == ix::WebSocketMessageType::Error) {
                    bool hasClients = false;
                    {
                        std::lock_guard<std::mutex> lock(_clientsMutex);
                        _activeClients.erase(connectionId(connectionState));
                        hasClients = !_activeClients.empty();
                    }
                    _hasConnection.store(hasClients);
                    return;
                }
                if (message->type != ix::WebSocketMessageType::Message || message->binary) {
                    return;
                }
                std::lock_guard<std::mutex> lock(_rxMutex);
                _rxMessages.push(message->str);
            });
        const bool started = _server->listenAndStart();
        _open.store(started);
        if (!started) {
            _server.reset();
            _localPort = 0;
        }
    }

    void close() override {
        if (_server) {
            _server->stop();
            _server.reset();
        }
        _localPort = 0;
        _hasConnection.store(false);
        {
            std::lock_guard<std::mutex> lock(_clientsMutex);
            _activeClients.clear();
        }
        {
            std::lock_guard<std::mutex> lock(_rxMutex);
            std::queue<std::string> empty;
            _rxMessages.swap(empty);
        }
        _open.store(false);
        if (_netInitialized) {
            ix::uninitNetSystem();
            _netInitialized = false;
        }
    }

    void poll() override {
        if (!_open.load() || _sink == nullptr) {
            return;
        }

        while (true) {
            std::string message;
            {
                std::lock_guard<std::mutex> lock(_rxMutex);
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
        for (const auto& client : _server->getClients()) {
            if (client) {
                (void)client->sendText(text);
            }
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
        return _server != nullptr && _hasConnection.load();
    }

    std::uint64_t connectionGeneration() const {
        return _connectionGeneration.load(std::memory_order_relaxed);
    }

private:
    static std::string connectionId(const std::shared_ptr<ix::ConnectionState>& connectionState) {
        return connectionState ? connectionState->getId() : std::string();
    }

    std::uint16_t _port = 0;
    std::uint16_t _localPort = 0;
    std::string _address;
    std::unique_ptr<ix::WebSocketServer> _server;
    IByteSink* _sink = nullptr;
    std::atomic<bool> _open{false};
    std::atomic<bool> _hasConnection{false};
    std::atomic<std::uint64_t> _connectionGeneration{0};
    bool _netInitialized = false;
    mutable std::mutex _clientsMutex;
    std::set<std::string> _activeClients;
    mutable std::mutex _rxMutex;
    std::queue<std::string> _rxMessages;
};

}  // namespace axtp
