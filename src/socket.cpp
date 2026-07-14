#include "socket.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>

#ifdef _WIN32
# include <ws2tcpip.h>
#else
# include <arpa/inet.h>
# include <fcntl.h>
# include <netdb.h>
# include <netinet/tcp.h>
# include <sys/select.h>
# include <sys/socket.h>
# include <unistd.h>
#endif

namespace qb {

#ifdef _WIN32
static bool ensure_winsock() {
    static std::once_flag once;
    static bool initialized = false;
    std::call_once(once, [] {
        WSADATA data{};
        initialized = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return initialized;
}
static int last_socket_error() { return WSAGetLastError(); }
static bool is_in_progress(int e) {
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}
static void close_native(qb_native_socket s) { closesocket(s); }
#else
static bool ensure_winsock() { return true; }
static int last_socket_error() { return errno; }
static bool is_in_progress(int e) { return e == EINPROGRESS; }
static void close_native(qb_native_socket s) { ::close(s); }
#endif

Socket::~Socket() { close(); }

Socket::Socket(Socket &&other) noexcept : handle_(other.handle_) {
    other.handle_ = qb_invalid_socket;
}

Socket &Socket::operator=(Socket &&other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = qb_invalid_socket;
    }
    return *this;
}

qb_result Socket::connect(const std::string &host, std::uint16_t port,
                          std::uint32_t timeout_ms) {
    close();
    if (!ensure_winsock() || host.empty()) {
        return QB_E_INVALID;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo *addresses = nullptr;
    const auto service = std::to_string(port ? port : QB_USBIP_DEFAULT_PORT);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses) != 0) {
        return QB_E_NETWORK;
    }

    qb_result result = QB_E_NETWORK;
    for (auto *address = addresses; address; address = address->ai_next) {
        auto candidate = ::socket(address->ai_family, address->ai_socktype,
                                  address->ai_protocol);
        if (candidate == qb_invalid_socket) {
            continue;
        }
#ifdef _WIN32
        u_long nonblocking = 1;
        ioctlsocket(candidate, FIONBIO, &nonblocking);
#else
        const int flags = fcntl(candidate, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(candidate, F_SETFL, flags | O_NONBLOCK);
        }
#endif
#ifdef _WIN32
        const int rc = ::connect(candidate, address->ai_addr,
                                 static_cast<int>(address->ai_addrlen));
#else
        const int rc = ::connect(candidate, address->ai_addr,
                                 static_cast<socklen_t>(address->ai_addrlen));
#endif
        if (rc != 0 && !is_in_progress(last_socket_error())) {
            close_native(candidate);
            continue;
        }
        if (rc != 0) {
            fd_set write_set;
            FD_ZERO(&write_set);
            FD_SET(candidate, &write_set);
            timeval timeout{};
            timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(timeout_ms / 1000);
            timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(
                (timeout_ms % 1000) * 1000);
#ifdef _WIN32
            const int selected = select(0, nullptr, &write_set, nullptr, &timeout);
            int socket_error = 0;
            int error_length = sizeof(socket_error);
#else
            const int selected = select(candidate + 1, nullptr,
                                        &write_set, nullptr, &timeout);
            int socket_error = 0;
            socklen_t error_length = sizeof(socket_error);
#endif
            if (selected <= 0 ||
                getsockopt(candidate, SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char *>(&socket_error),
                           &error_length) != 0 || socket_error != 0) {
                close_native(candidate);
                continue;
            }
        }
#ifdef _WIN32
        nonblocking = 0;
        ioctlsocket(candidate, FIONBIO, &nonblocking);
#else
        if (flags >= 0) {
            fcntl(candidate, F_SETFL, flags);
        }
#endif
        const int one = 1;
        setsockopt(candidate, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char *>(&one), sizeof(one));
        setsockopt(candidate, SOL_SOCKET, SO_KEEPALIVE,
                   reinterpret_cast<const char *>(&one), sizeof(one));
        handle_ = candidate;
        result = QB_OK;
        break;
    }
    freeaddrinfo(addresses);
    return result;
}

qb_result Socket::send_all(const void *data, std::size_t length) {
    if (!valid() || (!data && length)) {
        return QB_E_INVALID;
    }
    auto *p = static_cast<const char *>(data);
    while (length) {
        const auto chunk = static_cast<int>(std::min<std::size_t>(
            length, static_cast<std::size_t>(std::numeric_limits<int>::max())));
#ifdef _WIN32
        const auto sent = ::send(handle_, p, chunk, 0);
#else
        const auto sent = ::send(handle_, p, static_cast<std::size_t>(chunk), 0);
#endif
        if (sent <= 0) {
            return QB_E_DISCONNECTED;
        }
        p += sent;
        length -= static_cast<std::size_t>(sent);
    }
    return QB_OK;
}

qb_result Socket::recv_all(void *data, std::size_t length) {
    if (!valid() || (!data && length)) {
        return QB_E_INVALID;
    }
    auto *p = static_cast<char *>(data);
    while (length) {
        const auto chunk = static_cast<int>(std::min<std::size_t>(
            length, static_cast<std::size_t>(std::numeric_limits<int>::max())));
#ifdef _WIN32
        const auto received = ::recv(handle_, p, chunk, 0);
#else
        const auto received = ::recv(handle_, p,
                                     static_cast<std::size_t>(chunk), 0);
#endif
        if (received <= 0) {
            return QB_E_DISCONNECTED;
        }
        p += received;
        length -= static_cast<std::size_t>(received);
    }
    return QB_OK;
}

void Socket::shutdown() {
    if (!valid()) {
        return;
    }
#ifdef _WIN32
    ::shutdown(handle_, SD_BOTH);
#else
    ::shutdown(handle_, SHUT_RDWR);
#endif
}

void Socket::close() {
    if (valid()) {
        close_native(handle_);
        handle_ = qb_invalid_socket;
    }
}

bool is_loopback_host(const std::string &host) {
    if (host == "localhost" || host == "127.0.0.1" || host == "::1" ||
        host == "[::1]") {
        return true;
    }
    if (!ensure_winsock()) {
        return false;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *addresses = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &addresses) != 0) {
        return false;
    }
    bool result = addresses != nullptr;
    for (auto *a = addresses; a; a = a->ai_next) {
        if (a->ai_family == AF_INET) {
            const auto *v4 = reinterpret_cast<const sockaddr_in *>(a->ai_addr);
            result = result && ((ntohl(v4->sin_addr.s_addr) >> 24) == 127);
        } else if (a->ai_family == AF_INET6) {
            const auto *v6 = reinterpret_cast<const sockaddr_in6 *>(a->ai_addr);
            static const std::array<std::uint8_t, 16> loopback =
                {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
            result = result &&
                std::memcmp(&v6->sin6_addr, loopback.data(), loopback.size()) == 0;
        } else {
            result = false;
        }
    }
    freeaddrinfo(addresses);
    return result;
}

} // namespace qb
