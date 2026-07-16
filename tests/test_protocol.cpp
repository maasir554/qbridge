#include "test.hpp"

#include "protocol.hpp"

#include <array>
#include <cstring>
#include <vector>

void test_protocol() {
    std::array<std::uint8_t, 4> integer{};
    qb::wire::put_u32(integer.data(), 0x12345678);
    CHECK(integer[0] == 0x12 && integer[1] == 0x34 && integer[2] == 0x56 &&
          integer[3] == 0x78);
    CHECK(qb::wire::get_u32(integer.data()) == 0x12345678);
    qb::wire::put_i32(integer.data(), -104);
    CHECK(qb::wire::get_i32(integer.data()) == -104);

    const auto op = qb::wire::make_op(qb::wire::op_req_devlist);
    CHECK(op[0] == 0x01 && op[1] == 0x11 && op[2] == 0x80 && op[3] == 0x05);
    std::uint32_t status = 99;
    auto reply = qb::wire::make_op(qb::wire::op_rep_devlist);
    CHECK(qb::wire::parse_op(reply.data(), reply.size(),
                             qb::wire::op_rep_devlist, &status) == QB_OK);
    CHECK(status == 0);

    qb_transfer transfer{};
    transfer.type = QB_TRANSFER_CONTROL;
    transfer.direction = QB_DIR_IN;
    transfer.endpoint = 0;
    transfer.length = 18;
    const std::uint8_t setup[8] = {0x80, 6, 0, 1, 0, 0, 18, 0};
    std::memcpy(transfer.setup, setup, sizeof(setup));
    const auto submit = qb::wire::make_submit(7, 0x0001000f, transfer);
    CHECK(qb::wire::get_u32(submit.data()) == qb::wire::cmd_submit);
    CHECK(qb::wire::get_u32(submit.data() + 4) == 7);
    CHECK(qb::wire::get_u32(submit.data() + 8) == 0x0001000f);
    CHECK(qb::wire::get_u32(submit.data() + 12) == QB_DIR_IN);
    CHECK(qb::wire::get_u32(submit.data() + 24) == 18);
    CHECK(qb::wire::get_u32(submit.data() + 32) == 0);
    CHECK(std::memcmp(submit.data() + 40, setup, 8) == 0);

    const auto unlink = qb::wire::make_unlink(8, 0x0001000f, 7);
    CHECK(qb::wire::get_u32(unlink.data()) == qb::wire::cmd_unlink);
    CHECK(qb::wire::get_u32(unlink.data() + 4) == 8);
    CHECK(qb::wire::get_u32(unlink.data() + 20) == 7);

    qb_iso_packet packets[2] = {{0, 64, 60, 0}, {80, 32, 0, -18}};
    std::vector<std::uint8_t> encoded;
    qb::wire::append_iso_descriptors(encoded, packets, 2);
    CHECK(encoded.size() == 32);
    qb_iso_packet decoded[2]{};
    CHECK(qb::wire::parse_iso_descriptors(encoded.data(), encoded.size(),
                                           decoded, 2) == QB_OK);
    CHECK(decoded[0].offset == 0 && decoded[0].length == 64 &&
          decoded[0].actual_length == 60 && decoded[0].status == 0);
    CHECK(decoded[1].offset == 80 && decoded[1].length == 32 &&
          decoded[1].status == -18);

    std::array<std::uint8_t, qb::wire::device_size> raw_device{};
    std::memcpy(raw_device.data(), "/sys/device", 11);
    std::memcpy(raw_device.data() + 256, "1-7", 3);
    qb::wire::put_u32(raw_device.data() + 288, 1);
    qb::wire::put_u32(raw_device.data() + 292, 15);
    qb::wire::put_u32(raw_device.data() + 296, QB_SPEED_HIGH);
    qb::wire::put_u16(raw_device.data() + 300, 0x046d);
    qb::wire::put_u16(raw_device.data() + 302, 0x0825);
    raw_device[311] = 2;
    qb_device_info device{};
    CHECK(qb::wire::parse_device(raw_device.data(), raw_device.size(),
                                  &device) == QB_OK);
    CHECK(std::strcmp(device.busid, "1-7") == 0);
    CHECK(device.busnum == 1 && device.devnum == 15);
    CHECK(device.vendor_id == 0x046d && device.product_id == 0x0825);
    CHECK(device.num_interfaces == 2);
}
