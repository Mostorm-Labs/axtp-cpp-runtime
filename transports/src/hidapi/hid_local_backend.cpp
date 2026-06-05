#include "hidapi/hid_local_backend.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#    include <fcntl.h>
#    include <sys/socket.h>
#    include <sys/un.h>
#    include <unistd.h>
#endif

namespace axtp {

struct LocalHidBackend::Impl {
#if !defined(_WIN32)
    int listenFd = -1;
    int peerFd = -1;
#endif
    bool open = false;
    std::vector<Byte> pending;
};

std::unique_ptr<IHidBackend> LocalHidBackend::client(std::string path) {
    return std::unique_ptr<IHidBackend>(new LocalHidBackend(Mode::Client, std::move(path)));
}

std::unique_ptr<IHidBackend> LocalHidBackend::server(std::string path) {
    return std::unique_ptr<IHidBackend>(new LocalHidBackend(Mode::Server, std::move(path)));
}

LocalHidBackend::LocalHidBackend(Mode mode, std::string path)
    : _mode(mode)
    , _path(std::move(path))
    , _impl(std::make_unique<Impl>()) {}

LocalHidBackend::~LocalHidBackend() {
    close();
}

#if defined(_WIN32)

bool LocalHidBackend::open(const HidTransportOptions&) {
    return false;
}

void LocalHidBackend::close() {
    _impl->open = false;
    _impl->pending.clear();
}

bool LocalHidBackend::writeReport(const Byte*, std::size_t) {
    return false;
}

std::optional<std::size_t> LocalHidBackend::readReport(Byte*, std::size_t) {
    return std::nullopt;
}

#else

namespace {

bool setNonBlocking(int fd) {
    const auto flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void suppressSigPipe(int fd) {
#    if defined(SO_NOSIGPIPE)
    int enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#    else
    (void)fd;
#    endif
}

int sendFlags() {
#    if defined(MSG_NOSIGNAL)
    return MSG_NOSIGNAL;
#    else
    return 0;
#    endif
}

bool fillAddress(const std::string& path, sockaddr_un* address) {
    if (path.empty() || path.size() >= sizeof(address->sun_path)) {
        return false;
    }
    std::memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    std::memcpy(address->sun_path, path.c_str(), path.size() + 1);
    return true;
}

void closeFd(int* fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

}  // namespace

bool LocalHidBackend::open(const HidTransportOptions&) {
    close();

    sockaddr_un address{};
    if (!fillAddress(_path, &address)) {
        return false;
    }

    if (_mode == Mode::Server) {
        _impl->listenFd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (_impl->listenFd < 0) {
            return false;
        }
        suppressSigPipe(_impl->listenFd);
        unlink(_path.c_str());
        if (bind(_impl->listenFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            close();
            return false;
        }
        if (listen(_impl->listenFd, 1) != 0 || !setNonBlocking(_impl->listenFd)) {
            close();
            return false;
        }
        _impl->open = true;
        return true;
    }

    _impl->peerFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (_impl->peerFd < 0) {
        return false;
    }
    suppressSigPipe(_impl->peerFd);
    if (connect(_impl->peerFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close();
        return false;
    }
    if (!setNonBlocking(_impl->peerFd)) {
        close();
        return false;
    }
    _impl->open = true;
    return true;
}

void LocalHidBackend::close() {
    closeFd(&_impl->peerFd);
    closeFd(&_impl->listenFd);
    if (_mode == Mode::Server && !_path.empty()) {
        unlink(_path.c_str());
    }
    _impl->pending.clear();
    _impl->open = false;
}

bool LocalHidBackend::writeReport(const Byte* data, std::size_t size) {
    if (!_impl->open || data == nullptr || size == 0) {
        return false;
    }
    if (_mode == Mode::Server && _impl->peerFd < 0) {
        readReport(nullptr, 0);
    }
    if (_impl->peerFd < 0) {
        return false;
    }

    std::size_t offset = 0;
    int retries = 0;
    while (offset < size) {
        const auto sent = send(_impl->peerFd, data + offset, size - offset, sendFlags());
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            retries = 0;
            continue;
        }
        if (sent == 0) {
            return false;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            if (++retries > 100) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        closeFd(&_impl->peerFd);
        return false;
    }
    return true;
}

std::optional<std::size_t> LocalHidBackend::readReport(Byte* data, std::size_t size) {
    if (!_impl->open) {
        return std::nullopt;
    }

    if (_mode == Mode::Server && _impl->peerFd < 0) {
        const auto fd = accept(_impl->listenFd, nullptr, nullptr);
        if (fd >= 0) {
            _impl->peerFd = fd;
            suppressSigPipe(_impl->peerFd);
            setNonBlocking(_impl->peerFd);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            return std::nullopt;
        }
    }
    if (_impl->peerFd < 0) {
        return 0;
    }

    Byte buffer[4096];
    while (true) {
        const auto received = recv(_impl->peerFd, buffer, sizeof(buffer), 0);
        if (received > 0) {
            _impl->pending.insert(_impl->pending.end(), buffer, buffer + received);
            continue;
        }
        if (received == 0) {
            closeFd(&_impl->peerFd);
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            break;
        }
        closeFd(&_impl->peerFd);
        return std::nullopt;
    }

    if (data == nullptr || size == 0 || _impl->pending.size() < size) {
        return 0;
    }

    std::copy(
        _impl->pending.begin(), _impl->pending.begin() + static_cast<std::ptrdiff_t>(size), data);
    _impl->pending.erase(_impl->pending.begin(),
                         _impl->pending.begin() + static_cast<std::ptrdiff_t>(size));
    return size;
}

#endif

}  // namespace axtp
