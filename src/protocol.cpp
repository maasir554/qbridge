#include "protocol.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace qb::wire {

std::uint16_t get_u16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>((std::uint16_t{p[0]} << 8) | p[1]);
}

std::uint32_t get_u32(const std::uint8_t *p) {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) |
           (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
}

std::int32_t get_i32(const std::uint8_t *p) {
    return static_cast<std::int32_t>(get_u32(p));
}

void put_u16(std::uint8_t *p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}

void put_u32(std::uint8_t *p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

void put_i32(std::uint8_t *p, std::int32_t v) {
    put_u32(p, static_cast<std::uint32_t>(v));
}

std::array<std::uint8_t, op_header_size> make_op(std::uint16_t code) {
    std::array<std::uint8_t, op_header_size> result{};
    put_u16(result.data(), version);
    put_u16(result.data() + 2, code);
    return result;
}

qb_result parse_op(const std::uint8_t *data, std::size_t length,
                   std::uint16_t expected_code, std::uint32_t *status) {
    if (!data || length != op_header_size || !status) {
        return QB_E_INVALID;
    }
    if (get_u16(data) != version || get_u16(data + 2) != expected_code) {
        return QB_E_PROTOCOL;
    }
    *status = get_u32(data + 4);
    return QB_OK;
}

static void copy_wire_string(char *out, std::size_t out_size,
                             const std::uint8_t *in, std::size_t in_size) {
    const auto end = std::find(in, in + in_size, std::uint8_t{0});
    const auto n = std::min<std::size_t>(static_cast<std::size_t>(end - in),
                                         out_size - 1);
    std::memcpy(out, in, n);
    out[n] = '\0';
}

qb_result parse_device(const std::uint8_t *data, std::size_t length,
                       qb_device_info *device) {
    if (!data || !device || length < device_size) {
        return QB_E_INVALID;
    }
    std::memset(device, 0, sizeof(*device));
    copy_wire_string(device->path, sizeof(device->path), data, 256);
    copy_wire_string(device->busid, sizeof(device->busid), data + 256, 32);
    device->busnum = get_u32(data + 288);
    device->devnum = get_u32(data + 292);
    device->speed = static_cast<qb_speed>(get_u32(data + 296));
    device->vendor_id = get_u16(data + 300);
    device->product_id = get_u16(data + 302);
    device->bcd_device = get_u16(data + 304);
    device->device_class = data[306];
    device->device_subclass = data[307];
    device->device_protocol = data[308];
    device->configuration_value = data[309];
    device->num_configurations = data[310];
    device->num_interfaces = data[311];
    if (device->num_interfaces > QB_MAX_INTERFACES) {
        return QB_E_LIMIT;
    }
    return QB_OK;
}

std::array<std::uint8_t, urb_header_size>
make_submit(std::uint32_t seqnum, std::uint32_t devid,
            const qb_transfer &transfer) {
    std::array<std::uint8_t, urb_header_size> result{};
    put_u32(result.data(), cmd_submit);
    put_u32(result.data() + 4, seqnum);
    put_u32(result.data() + 8, devid);
    put_u32(result.data() + 12, static_cast<std::uint32_t>(transfer.direction));
    put_u32(result.data() + 16, transfer.endpoint);
    put_u32(result.data() + 20, transfer.flags);
    put_u32(result.data() + 24, transfer.length);
    put_i32(result.data() + 28, transfer.start_frame);
    put_u32(result.data() + 32,
            transfer.type == QB_TRANSFER_ISOCHRONOUS
                ? transfer.iso_packet_count
                : std::numeric_limits<std::uint32_t>::max());
    put_u32(result.data() + 36, transfer.interval);
    if (transfer.type == QB_TRANSFER_CONTROL) {
        std::memcpy(result.data() + 40, transfer.setup, 8);
    }
    return result;
}

std::array<std::uint8_t, urb_header_size>
make_unlink(std::uint32_t seqnum, std::uint32_t devid,
            std::uint32_t target_seqnum) {
    std::array<std::uint8_t, urb_header_size> result{};
    put_u32(result.data(), cmd_unlink);
    put_u32(result.data() + 4, seqnum);
    put_u32(result.data() + 8, devid);
    put_u32(result.data() + 20, target_seqnum);
    return result;
}

void append_iso_descriptors(std::vector<std::uint8_t> &out,
                            const qb_iso_packet *packets, std::uint32_t count) {
    const auto old_size = out.size();
    out.resize(old_size + static_cast<std::size_t>(count) * iso_descriptor_size);
    auto *p = out.data() + old_size;
    for (std::uint32_t i = 0; i < count; ++i, p += iso_descriptor_size) {
        put_u32(p, packets[i].offset);
        put_u32(p + 4, packets[i].length);
        put_u32(p + 8, packets[i].actual_length);
        put_i32(p + 12, packets[i].status);
    }
}

qb_result parse_iso_descriptors(const std::uint8_t *data, std::size_t length,
                                qb_iso_packet *packets, std::uint32_t count) {
    if ((!data && count) || (!packets && count) ||
        length != static_cast<std::size_t>(count) * iso_descriptor_size) {
        return QB_E_INVALID;
    }
    for (std::uint32_t i = 0; i < count; ++i, data += iso_descriptor_size) {
        packets[i].offset = get_u32(data);
        packets[i].length = get_u32(data + 4);
        packets[i].actual_length = get_u32(data + 8);
        packets[i].status = get_i32(data + 12);
    }
    return QB_OK;
}

} // namespace qb::wire
