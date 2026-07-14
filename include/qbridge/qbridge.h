#ifndef QBRIDGE_QBRIDGE_H
#define QBRIDGE_QBRIDGE_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(QBRIDGE_SHARED)
# ifdef QBRIDGE_BUILDING_LIBRARY
#  define QB_API __declspec(dllexport)
# else
#  define QB_API __declspec(dllimport)
# endif
#else
# define QB_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define QB_USBIP_DEFAULT_PORT 3240u
#define QB_BUSID_MAX 31u
#define QB_PATH_MAX 255u
#define QB_MAX_INTERFACES 32u
#define QB_MAX_ISO_PACKETS 1024u
#define QB_MAX_TRANSFER_SIZE (64u * 1024u * 1024u)

typedef enum qb_result {
    QB_OK = 0,
    QB_E_INVALID = -1,
    QB_E_NOMEM = -2,
    QB_E_NETWORK = -3,
    QB_E_PROTOCOL = -4,
    QB_E_NOT_FOUND = -5,
    QB_E_BUSY = -6,
    QB_E_DENIED = -7,
    QB_E_DISCONNECTED = -8,
    QB_E_CANCELED = -9,
    QB_E_LIMIT = -10,
    QB_E_INTERNAL = -11
} qb_result;

typedef enum qb_speed {
    QB_SPEED_UNKNOWN = 0,
    QB_SPEED_LOW = 1,
    QB_SPEED_FULL = 2,
    QB_SPEED_HIGH = 3,
    QB_SPEED_WIRELESS = 4,
    QB_SPEED_SUPER = 5,
    QB_SPEED_SUPER_PLUS = 6
} qb_speed;

typedef enum qb_direction {
    QB_DIR_OUT = 0,
    QB_DIR_IN = 1
} qb_direction;

typedef enum qb_transfer_type {
    QB_TRANSFER_CONTROL = 0,
    QB_TRANSFER_ISOCHRONOUS = 1,
    QB_TRANSFER_BULK = 2,
    QB_TRANSFER_INTERRUPT = 3
} qb_transfer_type;

typedef struct qb_interface_info {
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
} qb_interface_info;

typedef struct qb_device_info {
    char path[QB_PATH_MAX + 1];
    char busid[QB_BUSID_MAX + 1];
    uint32_t busnum;
    uint32_t devnum;
    qb_speed speed;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t bcd_device;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t configuration_value;
    uint8_t num_configurations;
    uint8_t num_interfaces;
    qb_interface_info interfaces[QB_MAX_INTERFACES];
} qb_device_info;

typedef struct qb_iso_packet {
    uint32_t offset;
    uint32_t length;
    uint32_t actual_length;
    int32_t status;
} qb_iso_packet;

typedef struct qb_transfer {
    qb_transfer_type type;
    qb_direction direction;
    uint8_t endpoint;
    uint32_t flags;
    int32_t start_frame;
    uint32_t interval;
    uint8_t setup[8];
    uint8_t *data;
    uint32_t length;
    qb_iso_packet *iso_packets;
    uint32_t iso_packet_count;
    void *user;
} qb_transfer;

typedef struct qb_completion {
    uint32_t id;
    int32_t status;
    uint32_t actual_length;
    int32_t start_frame;
    uint32_t error_count;
} qb_completion;

typedef struct qb_descriptor_cache {
    const uint8_t *device;
    size_t device_length;
    const uint8_t *configuration;
    size_t configuration_length;
    const char *manufacturer;
    const char *product;
    const char *serial;
} qb_descriptor_cache;

typedef struct qb_policy {
    int allow_remote;
    uint16_t vendor_id;
    uint16_t product_id;
    const char *serial;
} qb_policy;

typedef struct qb_session_config {
    const char *host;
    uint16_t port;
    const char *busid;
    uint32_t connect_timeout_ms;
    uint32_t reconnect_delay_ms;
    int auto_reconnect;
    int start_disconnected;
    qb_policy policy;
} qb_session_config;

typedef struct qb_session qb_session;

typedef void (*qb_completion_cb)(qb_session *session,
                                 const qb_completion *completion,
                                 qb_transfer *transfer,
                                 void *opaque);
typedef void (*qb_connection_cb)(qb_session *session, int connected,
                                 qb_result reason, void *opaque);

QB_API const char *qb_result_string(qb_result result);

QB_API qb_result qb_list_devices(const char *host, uint16_t port,
                                 uint32_t timeout_ms,
                                 qb_device_info **devices, size_t *count);
QB_API void qb_free_device_list(qb_device_info *devices);

QB_API qb_result qb_session_open(const qb_session_config *config,
                                 qb_completion_cb completion,
                                 qb_connection_cb connection,
                                 void *opaque,
                                 qb_session **out_session);
QB_API void qb_session_close(qb_session *session);
QB_API qb_result qb_session_submit(qb_session *session, qb_transfer *transfer,
                                   uint32_t *out_id);
QB_API qb_result qb_session_cancel(qb_session *session, uint32_t transfer_id);
QB_API int qb_session_connected(const qb_session *session);
QB_API const qb_device_info *qb_session_device(const qb_session *session);
QB_API const qb_descriptor_cache *qb_session_descriptors(const qb_session *session);
QB_API qb_result qb_session_configuration(const qb_session *session,
                                          uint8_t index,
                                          const uint8_t **data,
                                          size_t *length);

#ifdef __cplusplus
}
#endif

#endif
