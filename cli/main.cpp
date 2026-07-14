#include "qbridge/qbridge.h"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

void usage() {
    std::fputs(
        "QBridge USB/IP client\n\n"
        "Usage:\n"
        "  qbridge list [--host HOST] [--port PORT] [--allow-remote]\n"
        "  qbridge probe --busid BUSID [--host HOST] [--port PORT] "
        "[--allow-remote]\n\n"
        "Remote hosts are rejected unless --allow-remote is specified.\n",
        stderr);
}

bool parse_port(const char *text, std::uint16_t *port) {
    unsigned value = 0;
    const auto end = text + std::strlen(text);
    const auto result = std::from_chars(text, end, value);
    if (result.ec != std::errc{} || result.ptr != end || value == 0 ||
        value > 65535) {
        return false;
    }
    *port = static_cast<std::uint16_t>(value);
    return true;
}

const char *speed_name(qb_speed speed) {
    switch (speed) {
    case QB_SPEED_LOW: return "1.5 Mb/s";
    case QB_SPEED_FULL: return "12 Mb/s";
    case QB_SPEED_HIGH: return "480 Mb/s";
    case QB_SPEED_WIRELESS: return "wireless";
    case QB_SPEED_SUPER: return "5 Gb/s";
    case QB_SPEED_SUPER_PLUS: return "10+ Gb/s";
    default: return "unknown";
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 2;
    }
    const std::string command(argv[1]);
    std::string host = "127.0.0.1";
    std::string busid;
    std::uint16_t port = QB_USBIP_DEFAULT_PORT;
    bool allow_remote = false;
    for (int i = 2; i < argc; ++i) {
        const std::string option(argv[i]);
        if (option == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (option == "--port" && i + 1 < argc) {
            if (!parse_port(argv[++i], &port)) {
                std::fputs("invalid port\n", stderr);
                return 2;
            }
        } else if (option == "--busid" && i + 1 < argc) {
            busid = argv[++i];
        } else if (option == "--allow-remote") {
            allow_remote = true;
        } else {
            usage();
            return 2;
        }
    }

    if (command == "list") {
        if (!allow_remote && host != "localhost" && host != "127.0.0.1" &&
            host != "::1" && host != "[::1]") {
            std::fputs("remote host rejected; use --allow-remote explicitly\n",
                       stderr);
            return 1;
        }
        qb_device_info *devices = nullptr;
        std::size_t count = 0;
        const auto result = qb_list_devices(host.c_str(), port, 5000,
                                            &devices, &count);
        if (result != QB_OK) {
            std::fprintf(stderr, "list failed: %s\n", qb_result_string(result));
            return 1;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const auto &d = devices[i];
            std::printf("%-12s %04x:%04x  %-10s class %02x/%02x/%02x\n",
                        d.busid, d.vendor_id, d.product_id, speed_name(d.speed),
                        d.device_class, d.device_subclass, d.device_protocol);
        }
        qb_free_device_list(devices);
        return 0;
    }

    if (command == "probe" && !busid.empty()) {
        qb_session_config config{};
        config.host = host.c_str();
        config.port = port;
        config.busid = busid.c_str();
        config.connect_timeout_ms = 5000;
        config.policy.allow_remote = allow_remote ? 1 : 0;
        qb_session *session = nullptr;
        const auto result = qb_session_open(&config, nullptr, nullptr, nullptr,
                                            &session);
        if (result != QB_OK) {
            std::fprintf(stderr, "probe failed: %s\n", qb_result_string(result));
            return 1;
        }
        const auto *device = qb_session_device(session);
        const auto *descriptors = qb_session_descriptors(session);
        std::printf("busid:        %s\n", device->busid);
        std::printf("VID:PID:      %04x:%04x\n", device->vendor_id,
                    device->product_id);
        std::printf("speed:        %s\n", speed_name(device->speed));
        std::printf("manufacturer: %s\n", descriptors->manufacturer);
        std::printf("product:      %s\n", descriptors->product);
        std::printf("serial:       %s\n", descriptors->serial);
        std::printf("config bytes: %zu\n", descriptors->configuration_length);
        qb_session_close(session);
        return 0;
    }

    usage();
    return 2;
}
