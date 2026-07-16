#ifndef QBRIDGE_MEDIA_H
#define QBRIDGE_MEDIA_H

#include <stdint.h>

#define QB_MEDIA_MAPPING_NAME "Local\\QBridgeCamera"
#define QB_MEDIA_MAGIC 0x51424d46u
#define QB_MEDIA_VERSION 1u
#define QB_MEDIA_MAX_FRAME (2u * 1024u * 1024u)

typedef struct qb_media_frame {
    volatile uint32_t sequence;
    uint32_t magic;
    uint32_t version;
    uint32_t header_size;
    uint32_t frame_size;
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint64_t timestamp_100ns;
    uint64_t frame_number;
    uint8_t data[QB_MEDIA_MAX_FRAME];
} qb_media_frame;

#endif
