#include "qbridge/qbridge.h"

#include "protocol.hpp"
#include "socket.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct Pending {
    qb_transfer *transfer = nullptr;
};

struct DescriptorStorage {
    std::vector<std::uint8_t> device;
    std::vector<std::uint8_t> configuration;
    std::vector<std::vector<std::uint8_t>> configurations;
    std::string manufacturer;
    std::string product;
    std::string serial;
    qb_descriptor_cache view{};

    void update_view() {
        view.device = device.empty() ? nullptr : device.data();
        view.device_length = device.size();
        view.configuration = configuration.empty() ? nullptr : configuration.data();
        view.configuration_length = configuration.size();
        view.manufacturer = manufacturer.c_str();
        view.product = product.c_str();
        view.serial = serial.c_str();
    }
};

} // namespace

struct qb_session {
    qb_session_config config{};
    std::string host;
    std::string busid;
    std::string serial_policy;
    qb::Socket socket;
    qb_device_info device{};
    DescriptorStorage descriptors;
    qb_completion_cb completion = nullptr;
    qb_connection_cb connection = nullptr;
    void *opaque = nullptr;
    std::atomic<bool> connected{false};
    std::atomic<bool> stopping{false};
    std::atomic<std::uint32_t> next_sequence{1};
    std::mutex write_mutex;
    std::mutex pending_mutex;
    std::unordered_map<std::uint32_t, Pending> pending;
    std::unordered_map<std::uint32_t, std::uint32_t> unlinks;
    std::thread receiver;
};

namespace {

qb_result send_op(qb::Socket &socket, std::uint16_t code) {
    const auto header = qb::wire::make_op(code);
    return socket.send_all(header.data(), header.size());
}

qb_result recv_op(qb::Socket &socket, std::uint16_t expected,
                  std::uint32_t *status) {
    std::array<std::uint8_t, qb::wire::op_header_size> header{};
    auto result = socket.recv_all(header.data(), header.size());
    if (result != QB_OK) {
        return result;
    }
    return qb::wire::parse_op(header.data(), header.size(), expected, status);
}

qb_result import_status(std::uint32_t status) {
    switch (status) {
    case 0: return QB_OK;
    case 1: return QB_E_NOT_FOUND;
    case 2: return QB_E_BUSY;
    case 3: return QB_E_NETWORK;
    case 4: return QB_E_NOT_FOUND;
    default: return QB_E_PROTOCOL;
    }
}

bool valid_busid(const std::string &busid) {
    if (busid.empty() || busid.size() > QB_BUSID_MAX) {
        return false;
    }
    return std::all_of(busid.begin(), busid.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '.' || c == ':';
    });
}

qb_result send_import(qb::Socket &socket, const std::string &busid,
                      qb_device_info *device) {
    auto result = send_op(socket, qb::wire::op_req_import);
    if (result != QB_OK) {
        return result;
    }
    std::array<std::uint8_t, 32> wire_busid{};
    std::memcpy(wire_busid.data(), busid.data(), busid.size());
    result = socket.send_all(wire_busid.data(), wire_busid.size());
    if (result != QB_OK) {
        return result;
    }
    std::uint32_t status = 0;
    result = recv_op(socket, qb::wire::op_rep_import, &status);
    if (result != QB_OK) {
        return result;
    }
    result = import_status(status);
    if (result != QB_OK) {
        return result;
    }
    std::array<std::uint8_t, qb::wire::device_size> wire_device{};
    result = socket.recv_all(wire_device.data(), wire_device.size());
    if (result != QB_OK) {
        return result;
    }
    return qb::wire::parse_device(wire_device.data(), wire_device.size(), device);
}

std::uint32_t device_id(const qb_device_info &device) {
    return (device.busnum << 16) | (device.devnum & 0xffffu);
}

qb_result receive_submit(qb::Socket &socket,
                         const std::array<std::uint8_t, 48> &header,
                         qb_transfer *transfer, qb_completion *completion) {
    const auto command = qb::wire::get_u32(header.data());
    if (command != qb::wire::ret_submit) {
        return QB_E_PROTOCOL;
    }
    completion->id = qb::wire::get_u32(header.data() + 4);
    completion->status = qb::wire::get_i32(header.data() + 20);
    completion->actual_length = qb::wire::get_u32(header.data() + 24);
    completion->start_frame = qb::wire::get_i32(header.data() + 28);
    const auto packet_count = qb::wire::get_u32(header.data() + 32);
    completion->error_count = qb::wire::get_u32(header.data() + 36);

    if (completion->actual_length > QB_MAX_TRANSFER_SIZE ||
        completion->actual_length > transfer->length) {
        return QB_E_PROTOCOL;
    }
    std::vector<std::uint8_t> compact_iso;
    if (transfer->direction == QB_DIR_IN && completion->actual_length) {
        if (!transfer->data) {
            return QB_E_PROTOCOL;
        }
        if (transfer->type != QB_TRANSFER_ISOCHRONOUS) {
            const auto result = socket.recv_all(transfer->data,
                                                completion->actual_length);
            if (result != QB_OK) {
                return result;
            }
        } else {
            compact_iso.resize(completion->actual_length);
            const auto result = socket.recv_all(compact_iso.data(),
                                                compact_iso.size());
            if (result != QB_OK) {
                return result;
            }
        }
    }
    if (transfer->type == QB_TRANSFER_ISOCHRONOUS) {
        if (packet_count != transfer->iso_packet_count ||
            packet_count > QB_MAX_ISO_PACKETS) {
            return QB_E_PROTOCOL;
        }
        std::vector<std::uint8_t> descriptors(
            static_cast<std::size_t>(packet_count) * qb::wire::iso_descriptor_size);
        auto result = socket.recv_all(descriptors.data(), descriptors.size());
        if (result != QB_OK) {
            return result;
        }
        result = qb::wire::parse_iso_descriptors(
            descriptors.data(), descriptors.size(), transfer->iso_packets,
            packet_count);
        if (result != QB_OK) {
            return result;
        }
        if (transfer->direction == QB_DIR_IN) {
            std::size_t source = 0;
            for (std::uint32_t i = 0; i < transfer->iso_packet_count; ++i) {
                const auto n = transfer->iso_packets[i].actual_length;
                if (source + n > compact_iso.size() ||
                    transfer->iso_packets[i].offset + n > transfer->length) {
                    return QB_E_PROTOCOL;
                }
                std::memcpy(transfer->data + transfer->iso_packets[i].offset,
                            compact_iso.data() + source, n);
                source += n;
            }
            if (source != compact_iso.size()) {
                return QB_E_PROTOCOL;
            }
        }
    }
    return QB_OK;
}

qb_result control_sync(qb_session *session, const std::uint8_t setup[8],
                       std::vector<std::uint8_t> *data) {
    qb_transfer transfer{};
    transfer.type = QB_TRANSFER_CONTROL;
    transfer.direction = (setup[0] & 0x80) ? QB_DIR_IN : QB_DIR_OUT;
    std::memcpy(transfer.setup, setup, 8);
    transfer.data = data->empty() ? nullptr : data->data();
    transfer.length = static_cast<std::uint32_t>(data->size());
    const auto sequence = session->next_sequence.fetch_add(1);
    const auto header = qb::wire::make_submit(sequence, device_id(session->device),
                                               transfer);
    auto result = session->socket.send_all(header.data(), header.size());
    if (result != QB_OK) {
        return result;
    }
    if (transfer.direction == QB_DIR_OUT && transfer.length) {
        result = session->socket.send_all(transfer.data, transfer.length);
        if (result != QB_OK) {
            return result;
        }
    }
    std::array<std::uint8_t, 48> reply{};
    result = session->socket.recv_all(reply.data(), reply.size());
    if (result != QB_OK) {
        return result;
    }
    qb_completion completion{};
    result = receive_submit(session->socket, reply, &transfer, &completion);
    if (result != QB_OK) {
        return result;
    }
    if (completion.id != sequence || completion.status != 0) {
        return QB_E_PROTOCOL;
    }
    data->resize(completion.actual_length);
    return QB_OK;
}

std::string decode_usb_string(const std::vector<std::uint8_t> &descriptor) {
    if (descriptor.size() < 2 || descriptor[1] != 3 ||
        descriptor[0] > descriptor.size()) {
        return {};
    }
    std::string result;
    const auto end = std::min<std::size_t>(descriptor[0], descriptor.size());
    for (std::size_t i = 2; i + 1 < end; i += 2) {
        std::uint32_t codepoint = descriptor[i] |
                                  (std::uint32_t{descriptor[i + 1]} << 8);
        if (codepoint >= 0xd800 && codepoint <= 0xdbff && i + 3 < end) {
            const std::uint32_t low = descriptor[i + 2] |
                                      (std::uint32_t{descriptor[i + 3]} << 8);
            if (low >= 0xdc00 && low <= 0xdfff) {
                codepoint = 0x10000 + ((codepoint - 0xd800) << 10) +
                            (low - 0xdc00);
                i += 2;
            }
        }
        if (codepoint < 0x80) {
            result.push_back(static_cast<char>(codepoint));
        } else if (codepoint < 0x800) {
            result.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint < 0x10000) {
            result.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            result.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            result.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            result.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        }
    }
    return result;
}

qb_result get_descriptor(qb_session *session, std::uint8_t type,
                         std::uint8_t index, std::uint16_t language,
                         std::size_t length, std::vector<std::uint8_t> *out) {
    if (length > 65535) {
        return QB_E_LIMIT;
    }
    out->assign(length, 0);
    std::uint8_t setup[8] = {0x80, 0x06, index, type,
                             static_cast<std::uint8_t>(language),
                             static_cast<std::uint8_t>(language >> 8),
                             static_cast<std::uint8_t>(length),
                             static_cast<std::uint8_t>(length >> 8)};
    return control_sync(session, setup, out);
}

qb_result cache_descriptors(qb_session *session) {
    auto &cache = session->descriptors;
    auto result = get_descriptor(session, 1, 0, 0, 18, &cache.device);
    if (result != QB_OK || cache.device.size() < 18) {
        return result == QB_OK ? QB_E_PROTOCOL : result;
    }
    cache.configurations.clear();
    for (std::uint8_t index = 0; index < cache.device[17]; ++index) {
        std::vector<std::uint8_t> config_header;
        result = get_descriptor(session, 2, index, 0, 9, &config_header);
        if (result != QB_OK || config_header.size() < 9) {
            return result == QB_OK ? QB_E_PROTOCOL : result;
        }
        const std::size_t config_length = config_header[2] |
                                          (std::size_t{config_header[3]} << 8);
        if (config_length < 9 || config_length > 65535) {
            return QB_E_PROTOCOL;
        }
        std::vector<std::uint8_t> configuration;
        result = get_descriptor(session, 2, index, 0, config_length,
                                &configuration);
        if (result != QB_OK) {
            return result;
        }
        cache.configurations.push_back(std::move(configuration));
    }
    cache.configuration = cache.configurations.empty()
                              ? std::vector<std::uint8_t>{}
                              : cache.configurations.front();

    std::uint16_t language = 0x0409;
    std::vector<std::uint8_t> languages;
    if (get_descriptor(session, 3, 0, 0, 255, &languages) == QB_OK &&
        languages.size() >= 4) {
        language = static_cast<std::uint16_t>(languages[2] |
                                              (std::uint16_t{languages[3]} << 8));
    }
    const auto get_string = [&](std::uint8_t index) -> std::string {
        if (!index) {
            return {};
        }
        std::vector<std::uint8_t> raw;
        if (get_descriptor(session, 3, index, language, 255, &raw) != QB_OK) {
            return {};
        }
        return decode_usb_string(raw);
    };
    cache.manufacturer = get_string(cache.device[14]);
    cache.product = get_string(cache.device[15]);
    cache.serial = get_string(cache.device[16]);
    cache.update_view();
    return QB_OK;
}

qb_result apply_policy(const qb_session *session) {
    const auto &policy = session->config.policy;
    if (policy.vendor_id && policy.vendor_id != session->device.vendor_id) {
        return QB_E_DENIED;
    }
    if (policy.product_id && policy.product_id != session->device.product_id) {
        return QB_E_DENIED;
    }
    if (!session->serial_policy.empty() &&
        session->serial_policy != session->descriptors.serial) {
        return QB_E_DENIED;
    }
    return QB_OK;
}

qb_result connect_import_cache(qb_session *session) {
    session->socket.close();
    auto result = session->socket.connect(session->host, session->config.port,
                                          session->config.connect_timeout_ms);
    if (result != QB_OK) {
        return result;
    }
    result = send_import(session->socket, session->busid, &session->device);
    if (result != QB_OK) {
        session->socket.close();
        return result;
    }
    result = cache_descriptors(session);
    if (result == QB_OK) {
        result = apply_policy(session);
    }
    if (result != QB_OK) {
        session->socket.close();
    }
    return result;
}

void complete_all(qb_session *session, qb_result reason) {
    std::vector<std::pair<std::uint32_t, qb_transfer *>> transfers;
    {
        std::lock_guard<std::mutex> lock(session->pending_mutex);
        for (const auto &[id, pending] : session->pending) {
            transfers.emplace_back(id, pending.transfer);
        }
        session->pending.clear();
        session->unlinks.clear();
    }
    if (session->completion) {
        for (const auto &[id, transfer] : transfers) {
            qb_completion completion{id, static_cast<std::int32_t>(reason),
                                     0, 0, 0};
            session->completion(session, &completion, transfer, session->opaque);
        }
    }
}

void receiver_main(qb_session *session) {
    while (!session->stopping.load()) {
        std::array<std::uint8_t, 48> header{};
        auto result = session->socket.recv_all(header.data(), header.size());
        if (result != QB_OK) {
            session->connected.store(false);
            complete_all(session, QB_E_DISCONNECTED);
            if (session->connection) {
                session->connection(session, 0, result, session->opaque);
            }
            if (session->stopping.load() || !session->config.auto_reconnect) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(
                session->config.reconnect_delay_ms));
            if (session->stopping.load()) {
                break;
            }
            {
                std::lock_guard<std::mutex> write_lock(session->write_mutex);
                result = connect_import_cache(session);
            }
            if (result != QB_OK) {
                continue;
            }
            session->connected.store(true);
            if (session->connection) {
                session->connection(session, 1, QB_OK, session->opaque);
            }
            continue;
        }

        const auto command = qb::wire::get_u32(header.data());
        const auto sequence = qb::wire::get_u32(header.data() + 4);
        if (command == qb::wire::ret_submit) {
            qb_transfer *transfer = nullptr;
            {
                std::lock_guard<std::mutex> lock(session->pending_mutex);
                const auto found = session->pending.find(sequence);
                if (found != session->pending.end()) {
                    transfer = found->second.transfer;
                }
            }
            if (!transfer) {
                session->socket.shutdown();
                continue;
            }
            qb_completion completion{};
            result = receive_submit(session->socket, header, transfer, &completion);
            if (result != QB_OK) {
                session->socket.shutdown();
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(session->pending_mutex);
                session->pending.erase(sequence);
            }
            if (session->completion) {
                session->completion(session, &completion, transfer, session->opaque);
            }
        } else if (command == qb::wire::ret_unlink) {
            {
                std::lock_guard<std::mutex> lock(session->pending_mutex);
                const auto unlink = session->unlinks.find(sequence);
                if (unlink != session->unlinks.end()) {
                    session->unlinks.erase(unlink);
                }
            }
        } else {
            session->socket.shutdown();
        }
    }
}

} // namespace

extern "C" {

const char *qb_result_string(qb_result result) {
    switch (result) {
    case QB_OK: return "success";
    case QB_E_INVALID: return "invalid argument";
    case QB_E_NOMEM: return "out of memory";
    case QB_E_NETWORK: return "network error";
    case QB_E_PROTOCOL: return "USB/IP protocol error";
    case QB_E_NOT_FOUND: return "device not found";
    case QB_E_BUSY: return "device busy";
    case QB_E_DENIED: return "denied by policy";
    case QB_E_DISCONNECTED: return "disconnected";
    case QB_E_CANCELED: return "canceled";
    case QB_E_LIMIT: return "protocol limit exceeded";
    case QB_E_INTERNAL: return "internal error";
    }
    return "unknown error";
}

qb_result qb_list_devices(const char *host, std::uint16_t port,
                          std::uint32_t timeout_ms,
                          qb_device_info **devices, std::size_t *count) {
    if (!host || !devices || !count) {
        return QB_E_INVALID;
    }
    *devices = nullptr;
    *count = 0;
    qb::Socket socket;
    auto result = socket.connect(host, port ? port : QB_USBIP_DEFAULT_PORT,
                                 timeout_ms ? timeout_ms : 5000);
    if (result != QB_OK) {
        return result;
    }
    result = send_op(socket, qb::wire::op_req_devlist);
    if (result != QB_OK) {
        return result;
    }
    std::uint32_t status = 0;
    result = recv_op(socket, qb::wire::op_rep_devlist, &status);
    if (result != QB_OK || status != 0) {
        return result != QB_OK ? result : QB_E_PROTOCOL;
    }
    std::array<std::uint8_t, 4> count_wire{};
    result = socket.recv_all(count_wire.data(), count_wire.size());
    if (result != QB_OK) {
        return result;
    }
    const auto n = qb::wire::get_u32(count_wire.data());
    if (n > 65536) {
        return QB_E_LIMIT;
    }
    std::unique_ptr<qb_device_info[]> list(new (std::nothrow) qb_device_info[n]);
    if (n && !list) {
        return QB_E_NOMEM;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        std::array<std::uint8_t, qb::wire::device_size> wire_device{};
        result = socket.recv_all(wire_device.data(), wire_device.size());
        if (result != QB_OK) {
            return result;
        }
        result = qb::wire::parse_device(wire_device.data(), wire_device.size(),
                                        &list[i]);
        if (result != QB_OK) {
            return result;
        }
        for (std::uint8_t j = 0; j < list[i].num_interfaces; ++j) {
            std::array<std::uint8_t, 4> interface_wire{};
            result = socket.recv_all(interface_wire.data(), interface_wire.size());
            if (result != QB_OK) {
                return result;
            }
            list[i].interfaces[j] = {interface_wire[0], interface_wire[1],
                                     interface_wire[2]};
        }
    }
    *devices = list.release();
    *count = n;
    return QB_OK;
}

void qb_free_device_list(qb_device_info *devices) { delete[] devices; }

qb_result qb_session_open(const qb_session_config *config,
                          qb_completion_cb completion,
                          qb_connection_cb connection, void *opaque,
                          qb_session **out_session) {
    if (!config || !config->host || !config->busid || !out_session) {
        return QB_E_INVALID;
    }
    *out_session = nullptr;
    const std::string host(config->host);
    const std::string busid(config->busid);
    if (!valid_busid(busid) || (!config->policy.allow_remote &&
                                !qb::is_loopback_host(host))) {
        return QB_E_DENIED;
    }
    auto session = std::unique_ptr<qb_session>(new (std::nothrow) qb_session);
    if (!session) {
        return QB_E_NOMEM;
    }
    session->config = *config;
    session->host = host;
    session->busid = busid;
    session->serial_policy = config->policy.serial ? config->policy.serial : "";
    session->config.host = session->host.c_str();
    session->config.busid = session->busid.c_str();
    session->config.policy.serial = session->serial_policy.empty()
                                        ? nullptr : session->serial_policy.c_str();
    session->config.port = config->port ? config->port : QB_USBIP_DEFAULT_PORT;
    session->config.connect_timeout_ms = config->connect_timeout_ms
                                             ? config->connect_timeout_ms : 5000;
    session->config.reconnect_delay_ms = config->reconnect_delay_ms
                                             ? config->reconnect_delay_ms : 1000;
    session->completion = completion;
    session->connection = connection;
    session->opaque = opaque;
    auto result = connect_import_cache(session.get());
    if (result != QB_OK) {
        const bool retryable = result == QB_E_NETWORK || result == QB_E_NOT_FOUND ||
                               result == QB_E_BUSY || result == QB_E_DISCONNECTED;
        if (!session->config.auto_reconnect ||
            !session->config.start_disconnected || !retryable) {
            return result;
        }
        session->socket.close();
    } else {
        session->connected.store(true);
    }
    try {
        session->receiver = std::thread(receiver_main, session.get());
    } catch (...) {
        session->socket.close();
        return QB_E_INTERNAL;
    }
    *out_session = session.release();
    return QB_OK;
}

void qb_session_close(qb_session *session) {
    if (!session) {
        return;
    }
    session->stopping.store(true);
    session->connected.store(false);
    {
        std::lock_guard<std::mutex> lock(session->write_mutex);
        session->socket.shutdown();
    }
    if (session->receiver.joinable() &&
        session->receiver.get_id() == std::this_thread::get_id()) {
        try {
            std::thread([session] {
                session->receiver.join();
                session->socket.close();
                complete_all(session, QB_E_DISCONNECTED);
                delete session;
            }).detach();
        } catch (...) {
            /* The session is stopped and safe; leak rather than self-destruct. */
        }
        return;
    }
    if (session->receiver.joinable()) {
        session->receiver.join();
    }
    session->socket.close();
    complete_all(session, QB_E_DISCONNECTED);
    delete session;
}

qb_result qb_session_submit(qb_session *session, qb_transfer *transfer,
                            std::uint32_t *out_id) {
    if (!session || !transfer || !out_id ||
        (transfer->length && !transfer->data) ||
        transfer->length > QB_MAX_TRANSFER_SIZE || transfer->endpoint > 15 ||
        (transfer->type == QB_TRANSFER_ISOCHRONOUS &&
         (!transfer->iso_packets || !transfer->iso_packet_count ||
          transfer->iso_packet_count > QB_MAX_ISO_PACKETS))) {
        return QB_E_INVALID;
    }
    if (!session->connected.load()) {
        return QB_E_DISCONNECTED;
    }
    const auto sequence = session->next_sequence.fetch_add(1);
    const auto header = qb::wire::make_submit(sequence, device_id(session->device),
                                               *transfer);
    std::vector<std::uint8_t> payload;
    if (transfer->direction == QB_DIR_OUT && transfer->length) {
        if (transfer->type == QB_TRANSFER_ISOCHRONOUS) {
            for (std::uint32_t i = 0; i < transfer->iso_packet_count; ++i) {
                const auto &packet = transfer->iso_packets[i];
                if (packet.offset + packet.length > transfer->length) {
                    return QB_E_INVALID;
                }
                payload.insert(payload.end(), transfer->data + packet.offset,
                               transfer->data + packet.offset + packet.length);
            }
        } else {
            payload.assign(transfer->data, transfer->data + transfer->length);
        }
    }
    if (transfer->type == QB_TRANSFER_ISOCHRONOUS) {
        qb::wire::append_iso_descriptors(payload, transfer->iso_packets,
                                         transfer->iso_packet_count);
    }
    {
        std::lock_guard<std::mutex> lock(session->pending_mutex);
        session->pending.emplace(sequence, Pending{transfer});
    }
    qb_result result;
    {
        std::lock_guard<std::mutex> lock(session->write_mutex);
        result = session->socket.send_all(header.data(), header.size());
        if (result == QB_OK && !payload.empty()) {
            result = session->socket.send_all(payload.data(), payload.size());
        }
    }
    if (result != QB_OK) {
        std::lock_guard<std::mutex> lock(session->pending_mutex);
        session->pending.erase(sequence);
        session->socket.shutdown();
        return result;
    }
    *out_id = sequence;
    return QB_OK;
}

qb_result qb_session_cancel(qb_session *session, std::uint32_t transfer_id) {
    if (!session || !transfer_id) {
        return QB_E_INVALID;
    }
    if (!session->connected.load()) {
        return QB_E_DISCONNECTED;
    }
    {
        std::lock_guard<std::mutex> lock(session->pending_mutex);
        if (session->pending.find(transfer_id) == session->pending.end()) {
            return QB_E_NOT_FOUND;
        }
    }
    const auto sequence = session->next_sequence.fetch_add(1);
    const auto header = qb::wire::make_unlink(sequence, device_id(session->device),
                                               transfer_id);
    {
        std::lock_guard<std::mutex> lock(session->pending_mutex);
        session->unlinks.emplace(sequence, transfer_id);
    }
    std::lock_guard<std::mutex> lock(session->write_mutex);
    const auto result = session->socket.send_all(header.data(), header.size());
    if (result != QB_OK) {
        session->socket.shutdown();
    }
    return result;
}

int qb_session_connected(const qb_session *session) {
    return session && session->connected.load() ? 1 : 0;
}

const qb_device_info *qb_session_device(const qb_session *session) {
    return session ? &session->device : nullptr;
}

const qb_descriptor_cache *qb_session_descriptors(const qb_session *session) {
    return session ? &session->descriptors.view : nullptr;
}

qb_result qb_session_configuration(const qb_session *session,
                                   std::uint8_t index,
                                   const std::uint8_t **data,
                                   std::size_t *length) {
    if (!session || !data || !length) {
        return QB_E_INVALID;
    }
    if (index >= session->descriptors.configurations.size()) {
        return QB_E_NOT_FOUND;
    }
    const auto &configuration = session->descriptors.configurations[index];
    *data = configuration.data();
    *length = configuration.size();
    return QB_OK;
}

} // extern "C"
