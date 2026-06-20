#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>

#if defined(_WIN32)
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <winsock2.h>
#    include <ws2tcpip.h>
#else
#    include <cerrno>
#    include <fcntl.h>
#    include <netdb.h>
#    include <netinet/in.h>
#    include <sys/select.h>
#    include <sys/socket.h>
#    include <sys/types.h>
#    include <unistd.h>
#endif

#include "core/runtime/transport/transport.hpp"

namespace axtp {

namespace tcp_native_detail {

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;

inline int lastSocketError() {
    return WSAGetLastError();
}

inline bool wouldBlock(int error) {
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEALREADY;
}

inline void closeSocket(SocketHandle& socket) {
    if (socket != kInvalidSocket) {
        closesocket(socket);
        socket = kInvalidSocket;
    }
}

class SocketRuntime {
public:
    SocketRuntime() {
        WSADATA data{};
        _ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }

    ~SocketRuntime() {
        if (_ok) {
            WSACleanup();
        }
    }

    bool ok() const {
        return _ok;
    }

private:
    bool _ok = false;
};

inline bool setNonBlocking(SocketHandle socket) {
    u_long mode = 1;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
}

#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;

inline int lastSocketError() {
    return errno;
}

inline bool wouldBlock(int error) {
    return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS || error == EALREADY;
}

inline void closeSocket(SocketHandle& socket) {
    if (socket != kInvalidSocket) {
        close(socket);
        socket = kInvalidSocket;
    }
}

class SocketRuntime {
public:
    bool ok() const {
        return true;
    }
};

inline bool setNonBlocking(SocketHandle socket) {
    const auto flags = fcntl(socket, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

inline bool isValid(SocketHandle socket) {
    return socket != kInvalidSocket;
}

inline bool waitWritable(SocketHandle socket, std::chrono::milliseconds timeout) {
    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(socket, &writeSet);
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    const auto result = select(static_cast<int>(socket + 1), nullptr, &writeSet, nullptr, &tv);
    if (result <= 0) {
        return false;
    }
    int error = 0;
#if defined(_WIN32)
    int length = sizeof(error);
#else
    socklen_t length = sizeof(error);
#endif
    if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) != 0) {
        return false;
    }
    return error == 0;
}

inline bool setReuseAddress(SocketHandle socket) {
    int enabled = 1;
    return setsockopt(
               socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled), sizeof(enabled)) ==
           0;
}

inline TransportProfile tcpProfile() {
    TransportProfile profile;
    profile.kind = TransportKind::Tcp;
    profile.wireMode = AxtpWireMode::FramedBinary;
    profile.defaultRpcEncoding = jsonBinaryRpcEncoding();
    profile.messageOriented = false;
    profile.supportsTextMessage = false;
    profile.supportsBinaryMessage = true;
    profile.preferredFrameSize = 4096;
    return profile;
}

class TcpByteStream {
public:
    ~TcpByteStream() {
        close();
    }

    void attach(SocketHandle socket) {
        close();
        _socket = socket;
    }

    SocketHandle detach() {
        auto socket = _socket;
        _socket = kInvalidSocket;
        _outgoing.clear();
        _writeOffset = 0;
        return socket;
    }

    void close() {
        closeSocket(_socket);
        _outgoing.clear();
        _writeOffset = 0;
    }

    bool valid() const {
        return isValid(_socket);
    }

    SocketHandle handle() const {
        return _socket;
    }

    void sendBytes(const Byte* data, std::size_t size) {
        if (data == nullptr || size == 0) {
            return;
        }
        _outgoing.emplace_back(data, data + size);
        flush();
    }

    void flush() {
        while (valid() && !_outgoing.empty()) {
            const auto& bytes = _outgoing.front();
            const auto* data = reinterpret_cast<const char*>(bytes.data() + _writeOffset);
            const auto remaining = bytes.size() - _writeOffset;
            const auto written = ::send(_socket, data, static_cast<int>(remaining), 0);
            if (written > 0) {
                _writeOffset += static_cast<std::size_t>(written);
                if (_writeOffset == bytes.size()) {
                    _outgoing.pop_front();
                    _writeOffset = 0;
                }
                continue;
            }
            if (written == 0 || wouldBlock(lastSocketError())) {
                return;
            }
            close();
            return;
        }
    }

    void readAvailable(IByteSink* sink, bool closeOnPeerClose = true) {
        if (!valid() || sink == nullptr) {
            return;
        }
        std::array<Byte, 4096> buffer{};
        while (valid()) {
            const auto received =
                ::recv(_socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
            if (received > 0) {
                sink->onBytes(buffer.data(), static_cast<std::size_t>(received));
                continue;
            }
            if (received == 0) {
                if (closeOnPeerClose) {
                    close();
                }
                return;
            }
            if (wouldBlock(lastSocketError())) {
                return;
            }
            close();
            return;
        }
    }

private:
    SocketHandle _socket = kInvalidSocket;
    std::deque<Bytes> _outgoing;
    std::size_t _writeOffset = 0;
};

}  // namespace tcp_native_detail

class TcpClientTransport : public ITransport {
public:
    TcpClientTransport(std::string host,
                       std::uint16_t port,
                       std::chrono::milliseconds timeout = std::chrono::milliseconds(3000))
        : _host(std::move(host))
        , _port(port)
        , _timeout(timeout) {}

    void bind(IByteSink& sink) override {
        _sink = &sink;
    }

    void open() override {
        close();
        if (!_runtime.ok()) {
            return;
        }
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* results = nullptr;
        const auto service = std::to_string(_port);
        if (getaddrinfo(_host.c_str(), service.c_str(), &hints, &results) != 0) {
            return;
        }
        std::unique_ptr<addrinfo, void (*)(addrinfo*)> guard(results, freeaddrinfo);
        for (auto* entry = results; entry != nullptr; entry = entry->ai_next) {
            auto socket =
                ::socket(entry->ai_family, entry->ai_socktype, static_cast<int>(entry->ai_protocol));
            if (!tcp_native_detail::isValid(socket)) {
                continue;
            }
            if (!tcp_native_detail::setNonBlocking(socket)) {
                tcp_native_detail::closeSocket(socket);
                continue;
            }
            const auto result = ::connect(socket, entry->ai_addr, static_cast<int>(entry->ai_addrlen));
            if (result == 0 || tcp_native_detail::wouldBlock(tcp_native_detail::lastSocketError())) {
                if (result == 0 || tcp_native_detail::waitWritable(socket, _timeout)) {
                    _stream.attach(socket);
                    _open = true;
                    return;
                }
            }
            tcp_native_detail::closeSocket(socket);
        }
    }

    void close() override {
        _stream.close();
        _open = false;
    }

    void poll() override {
        if (!_open) {
            return;
        }
        _stream.flush();
        _stream.readAvailable(_sink);
        if (!_stream.valid()) {
            _open = false;
        }
    }

    void sendBytes(const Byte* data, std::size_t size) override {
        if (!_open) {
            return;
        }
        _stream.sendBytes(data, size);
    }

    TransportProfile profile() const override {
        return tcp_native_detail::tcpProfile();
    }

    bool isOpen() const {
        return _open && _stream.valid();
    }

    std::uint16_t localPort() const {
        if (!_stream.valid()) {
            return 0;
        }
        sockaddr_storage storage{};
#if defined(_WIN32)
        int length = sizeof(storage);
#else
        socklen_t length = sizeof(storage);
#endif
        if (getsockname(_stream.handle(), reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
            return 0;
        }
        if (storage.ss_family == AF_INET) {
            return ntohs(reinterpret_cast<const sockaddr_in*>(&storage)->sin_port);
        }
        if (storage.ss_family == AF_INET6) {
            return ntohs(reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_port);
        }
        return 0;
    }

private:
    tcp_native_detail::SocketRuntime _runtime;
    std::string _host;
    std::uint16_t _port = 0;
    std::chrono::milliseconds _timeout;
    tcp_native_detail::TcpByteStream _stream;
    IByteSink* _sink = nullptr;
    bool _open = false;
};

class TcpServerTransport : public ITransport {
public:
    explicit TcpServerTransport(std::uint16_t port, std::string address = "127.0.0.1")
        : _address(std::move(address))
        , _port(port) {}

    void bind(IByteSink& sink) override {
        _sink = &sink;
    }

    void open() override {
        close();
        if (!_runtime.ok()) {
            return;
        }
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_PASSIVE;
        addrinfo* results = nullptr;
        const auto service = std::to_string(_port);
        if (getaddrinfo(_address.c_str(), service.c_str(), &hints, &results) != 0) {
            return;
        }
        std::unique_ptr<addrinfo, void (*)(addrinfo*)> guard(results, freeaddrinfo);
        for (auto* entry = results; entry != nullptr; entry = entry->ai_next) {
            auto socket =
                ::socket(entry->ai_family, entry->ai_socktype, static_cast<int>(entry->ai_protocol));
            if (!tcp_native_detail::isValid(socket)) {
                continue;
            }
            tcp_native_detail::setReuseAddress(socket);
            if (!tcp_native_detail::setNonBlocking(socket)) {
                tcp_native_detail::closeSocket(socket);
                continue;
            }
            if (::bind(socket, entry->ai_addr, static_cast<int>(entry->ai_addrlen)) != 0) {
                tcp_native_detail::closeSocket(socket);
                continue;
            }
            if (::listen(socket, 1) != 0) {
                tcp_native_detail::closeSocket(socket);
                continue;
            }
            _listenSocket = socket;
            _open = true;
            return;
        }
    }

    void close() override {
        _client.close();
        tcp_native_detail::closeSocket(_listenSocket);
        _open = false;
    }

    void poll() override {
        if (!_open) {
            return;
        }
        acceptOne();
        _client.flush();
        _client.readAvailable(_sink);
    }

    void sendBytes(const Byte* data, std::size_t size) override {
        if (!_open || !_client.valid()) {
            return;
        }
        _client.sendBytes(data, size);
    }

    TransportProfile profile() const override {
        return tcp_native_detail::tcpProfile();
    }

    bool isOpen() const {
        return _open && tcp_native_detail::isValid(_listenSocket);
    }

    bool hasConnection() const {
        return _client.valid();
    }

    std::uint16_t localPort() const {
        if (!tcp_native_detail::isValid(_listenSocket)) {
            return 0;
        }
        sockaddr_storage storage{};
#if defined(_WIN32)
        int length = sizeof(storage);
#else
        socklen_t length = sizeof(storage);
#endif
        if (getsockname(_listenSocket, reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
            return 0;
        }
        if (storage.ss_family == AF_INET) {
            return ntohs(reinterpret_cast<const sockaddr_in*>(&storage)->sin_port);
        }
        if (storage.ss_family == AF_INET6) {
            return ntohs(reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_port);
        }
        return 0;
    }

private:
    void acceptOne() {
        if (_client.valid()) {
            return;
        }
        sockaddr_storage storage{};
#if defined(_WIN32)
        int length = sizeof(storage);
#else
        socklen_t length = sizeof(storage);
#endif
        auto socket = ::accept(_listenSocket, reinterpret_cast<sockaddr*>(&storage), &length);
        if (!tcp_native_detail::isValid(socket)) {
            return;
        }
        if (!tcp_native_detail::setNonBlocking(socket)) {
            tcp_native_detail::closeSocket(socket);
            return;
        }
        _client.attach(socket);
    }

    tcp_native_detail::SocketRuntime _runtime;
    std::string _address;
    std::uint16_t _port = 0;
    tcp_native_detail::SocketHandle _listenSocket = tcp_native_detail::kInvalidSocket;
    tcp_native_detail::TcpByteStream _client;
    IByteSink* _sink = nullptr;
    bool _open = false;
};

using TcpTransport = TcpServerTransport;

}  // namespace axtp
