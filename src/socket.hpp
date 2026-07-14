#ifndef QBRIDGE_SOCKET_HPP
#define QBRIDGE_SOCKET_HPP

#include "qbridge/qbridge.h"

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef _WIN32
# include <winsock2.h>
using qb_native_socket = SOCKET;
constexpr qb_native_socket qb_invalid_socket = INVALID_SOCKET;
#else
using qb_native_socket = int;
constexpr qb_native_socket qb_invalid_socket = -1;
#endif

namespace qb {

class Socket {
public:
    Socket() = default;
    explicit Socket(qb_native_socket handle) : handle_(handle) {}
    ~Socket();
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    Socket(Socket &&other) noexcept;
    Socket &operator=(Socket &&other) noexcept;

    qb_result connect(const std::string &host, std::uint16_t port,
                      std::uint32_t timeout_ms);
    qb_result send_all(const void *data, std::size_t length);
    qb_result recv_all(void *data, std::size_t length);
    void shutdown();
    void close();
    bool valid() const { return handle_ != qb_invalid_socket; }
    qb_native_socket native_handle() const { return handle_; }

private:
    qb_native_socket handle_ = qb_invalid_socket;
};

bool is_loopback_host(const std::string &host);

} // namespace qb

#endif

