#ifndef QBRIDGE_PROTOCOL_HPP
#define QBRIDGE_PROTOCOL_HPP

#include "qbridge/qbridge.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qb::wire {

constexpr std::uint16_t version = 0x0111;
constexpr std::uint16_t op_req_import = 0x8003;
constexpr std::uint16_t op_rep_import = 0x0003;
constexpr std::uint16_t op_req_devlist = 0x8005;
constexpr std::uint16_t op_rep_devlist = 0x0005;
constexpr std::uint32_t cmd_submit = 0x00000001;
constexpr std::uint32_t cmd_unlink = 0x00000002;
constexpr std::uint32_t ret_submit = 0x00000003;
constexpr std::uint32_t ret_unlink = 0x00000004;
constexpr std::size_t op_header_size = 8;
constexpr std::size_t device_size = 312;
constexpr std::size_t urb_header_size = 48;
constexpr std::size_t iso_descriptor_size = 16;

std::uint16_t get_u16(const std::uint8_t *p);
std::uint32_t get_u32(const std::uint8_t *p);
std::int32_t get_i32(const std::uint8_t *p);
void put_u16(std::uint8_t *p, std::uint16_t v);
void put_u32(std::uint8_t *p, std::uint32_t v);
void put_i32(std::uint8_t *p, std::int32_t v);

std::array<std::uint8_t, op_header_size> make_op(std::uint16_t code);
qb_result parse_op(const std::uint8_t *data, std::size_t length,
                   std::uint16_t expected_code, std::uint32_t *status);
qb_result parse_device(const std::uint8_t *data, std::size_t length,
                       qb_device_info *device);
std::array<std::uint8_t, urb_header_size>
make_submit(std::uint32_t seqnum, std::uint32_t devid,
            const qb_transfer &transfer);
std::array<std::uint8_t, urb_header_size>
make_unlink(std::uint32_t seqnum, std::uint32_t devid,
            std::uint32_t target_seqnum);
void append_iso_descriptors(std::vector<std::uint8_t> &out,
                            const qb_iso_packet *packets, std::uint32_t count);
qb_result parse_iso_descriptors(const std::uint8_t *data, std::size_t length,
                                qb_iso_packet *packets, std::uint32_t count);

} // namespace qb::wire

#endif

