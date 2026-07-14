/*
 * QBridge USB/IP pass-through device for QEMU.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qom/object.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/thread.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"
#include "hw/usb/usb.h"

#include <qbridge/qbridge.h>

#define TYPE_USB_IP_DEVICE "usb-ip"
OBJECT_DECLARE_SIMPLE_TYPE(USBIPDevice, USB_IP_DEVICE)

#define USBIP_URB_SHORT_NOT_OK 0x00000001u
#define USBIP_URB_ISO_ASAP     0x00000002u

typedef struct USBIPRequest USBIPRequest;

struct USBIPDevice {
    USBDevice parent_obj;
    char *host;
    char *busid;
    char *serial;
    uint16_t port;
    uint32_t vendor_id;
    uint32_t product_id;
    uint32_t connect_timeout_ms;
    uint32_t reconnect_delay_ms;
    bool allow_remote;
    bool reconnect;
    bool wait;

    qb_session *session;
    QemuMutex lock;
    QTAILQ_HEAD(, USBIPRequest) requests;
    QEMUBH *connection_bh;
    bool closing;
    bool remote_connected;
    qb_result connection_reason;
    uint8_t interval[2][USB_MAX_ENDPOINTS + 1];
};

struct USBIPRequest {
    USBIPDevice *owner;
    USBPacket *packet;
    qb_transfer transfer;
    qb_iso_packet iso;
    qb_completion completion;
    uint8_t *buffer;
    uint8_t *control_data;
    int request;
    int value;
    int index;
    bool control;
    bool completed;
    uint32_t id;
    QEMUBH *bh;
    QTAILQ_ENTRY(USBIPRequest) next;
};

static int usb_ip_status(int32_t status)
{
    if (status == 0) {
        return USB_RET_SUCCESS;
    }
    switch (status) {
    case -32: /* EPIPE */
        return USB_RET_STALL;
    case -75: /* EOVERFLOW */
        return USB_RET_BABBLE;
    case -19: /* ENODEV */
    case QB_E_DISCONNECTED:
        return USB_RET_NODEV;
    default:
        return USB_RET_IOERROR;
    }
}

static int usb_ip_speed(qb_speed speed)
{
    switch (speed) {
    case QB_SPEED_LOW: return USB_SPEED_LOW;
    case QB_SPEED_FULL: return USB_SPEED_FULL;
    case QB_SPEED_HIGH:
    case QB_SPEED_WIRELESS: return USB_SPEED_HIGH;
    case QB_SPEED_SUPER:
    case QB_SPEED_SUPER_PLUS: return USB_SPEED_SUPER;
    default: return USB_SPEED_FULL;
    }
}

static const uint8_t *usb_ip_find_config(USBIPDevice *s, uint8_t value,
                                         size_t *length)
{
    const qb_device_info *device = qb_session_device(s->session);
    uint8_t i;

    for (i = 0; device && i < device->num_configurations; ++i) {
        const uint8_t *data;
        size_t size;

        if (qb_session_configuration(s->session, i, &data, &size) == QB_OK &&
            size >= 9 && data[1] == USB_DT_CONFIG && data[5] == value) {
            *length = size;
            return data;
        }
    }
    return NULL;
}

static void usb_ip_update_endpoints(USBIPDevice *s)
{
    USBDevice *dev = USB_DEVICE(s);
    const uint8_t *config;
    size_t length, offset;
    int selected_interface = -1;
    bool selected_alt = false;
    int last_endpoint = -1;
    int last_endpoint_pid = USB_TOKEN_OUT;
    int last_endpoint_type = USB_ENDPOINT_XFER_INVALID;

    memset(s->interval, 0, sizeof(s->interval));
    usb_ep_reset(dev);
    if (!dev->configuration) {
        return;
    }
    config = usb_ip_find_config(s, dev->configuration, &length);
    if (!config) {
        return;
    }

    for (offset = 0; offset + 2 <= length;) {
        const uint8_t descriptor_length = config[offset];
        const uint8_t descriptor_type = config[offset + 1];

        if (descriptor_length < 2 || offset + descriptor_length > length) {
            return;
        }
        if (descriptor_type == USB_DT_INTERFACE && descriptor_length >= 9) {
            last_endpoint = -1;
            selected_interface = config[offset + 2];
            selected_alt = selected_interface < USB_MAX_INTERFACES &&
                config[offset + 3] == dev->altsetting[selected_interface];
        } else if (descriptor_type == USB_DT_ENDPOINT &&
                   descriptor_length >= 7 && selected_alt) {
            const uint8_t address = config[offset + 2];
            const int pid = (address & USB_DIR_IN) ? USB_TOKEN_IN : USB_TOKEN_OUT;
            const unsigned ep = address & 0x0f;
            const unsigned dir = (address & USB_DIR_IN) ? 1 : 0;
            const uint16_t max_packet = config[offset + 4] |
                                        (config[offset + 5] << 8);

            if (ep && ep <= USB_MAX_ENDPOINTS) {
                usb_ep_set_type(dev, pid, ep, config[offset + 3] & 3);
                usb_ep_set_ifnum(dev, pid, ep, selected_interface);
                usb_ep_set_max_packet_size(dev, pid, ep, max_packet);
                usb_ep_set_halted(dev, pid, ep, false);
                s->interval[dir][ep] = config[offset + 6];
                last_endpoint = ep;
                last_endpoint_pid = pid;
                last_endpoint_type = config[offset + 3] & 3;
            }
        } else if (descriptor_type == USB_DT_ENDPOINT_COMPANION &&
                   descriptor_length >= 6 && selected_alt) {
            if (last_endpoint > 0 &&
                last_endpoint_type == USB_ENDPOINT_XFER_BULK) {
                usb_ep_set_max_streams(dev, last_endpoint_pid, last_endpoint,
                                       config[offset + 3]);
            }
        } else {
            last_endpoint = -1;
        }
        offset += descriptor_length;
    }
}

static void usb_ip_apply_control_state(USBIPRequest *r)
{
    USBIPDevice *s = r->owner;
    USBDevice *dev = USB_DEVICE(s);

    if (r->completion.status != 0) {
        return;
    }
    switch (r->request) {
    case DeviceOutRequest | USB_REQ_SET_CONFIGURATION:
        dev->configuration = r->value & 0xff;
        memset(dev->altsetting, 0, sizeof(dev->altsetting));
        usb_ip_update_endpoints(s);
        break;
    case InterfaceOutRequest | USB_REQ_SET_INTERFACE:
        if (r->index >= 0 && r->index < USB_MAX_INTERFACES) {
            dev->altsetting[r->index] = r->value & 0xff;
            usb_ip_update_endpoints(s);
        }
        break;
    case EndpointOutRequest | USB_REQ_CLEAR_FEATURE:
        if (r->value == 0) {
            const int pid = (r->index & USB_DIR_IN) ? USB_TOKEN_IN : USB_TOKEN_OUT;
            usb_ep_set_halted(dev, pid, r->index & 0x0f, false);
        }
        break;
    }
}

static void usb_ip_request_bh(void *opaque)
{
    USBIPRequest *r = opaque;
    USBIPDevice *s = r->owner;
    USBDevice *dev = USB_DEVICE(s);
    USBPacket *packet;

    qemu_mutex_lock(&s->lock);
    packet = r->packet;
    QTAILQ_REMOVE(&s->requests, r, next);
    qemu_mutex_unlock(&s->lock);

    if (packet) {
        packet->status = usb_ip_status(r->completion.status);
        packet->actual_length = 0;
        if (packet->status == USB_RET_SUCCESS &&
            r->transfer.direction == QB_DIR_IN && r->completion.actual_length) {
            if (r->control) {
                memcpy(r->control_data, r->buffer, r->completion.actual_length);
                packet->actual_length = r->completion.actual_length;
            } else {
                usb_packet_copy(packet, r->buffer, r->completion.actual_length);
            }
        } else if (packet->status == USB_RET_SUCCESS) {
            packet->actual_length = r->completion.actual_length;
        }
        if (r->control) {
            usb_ip_apply_control_state(r);
            usb_generic_async_ctrl_complete(dev, packet);
        } else {
            usb_packet_complete(dev, packet);
        }
    }
    qemu_bh_delete(r->bh);
    g_free(r->buffer);
    g_free(r);
}

static void usb_ip_completion(qb_session *session,
                              const qb_completion *completion,
                              qb_transfer *transfer, void *opaque)
{
    USBIPRequest *r = transfer->user;
    USBIPDevice *s = opaque;

    (void)session;
    qemu_mutex_lock(&s->lock);
    r->completion = *completion;
    r->completed = true;
    if (!s->closing) {
        qemu_bh_schedule(r->bh);
    }
    qemu_mutex_unlock(&s->lock);
}

static void usb_ip_connection_bh(void *opaque)
{
    USBIPDevice *s = opaque;
    USBDevice *dev = USB_DEVICE(s);
    bool connected;
    Error *local_err = NULL;

    qemu_mutex_lock(&s->lock);
    connected = s->remote_connected;
    qemu_mutex_unlock(&s->lock);
    if (s->closing) {
        return;
    }
    if (!connected && dev->attached) {
        usb_device_detach(dev);
    } else if (connected && !dev->attached) {
        const qb_device_info *info = qb_session_device(s->session);
        dev->speed = usb_ip_speed(info->speed);
        dev->speedmask = 1 << dev->speed;
        usb_ip_update_endpoints(s);
        usb_device_attach(dev, &local_err);
        if (local_err) {
            error_report_err(local_err);
        }
    }
}

static void usb_ip_connection(qb_session *session, int connected,
                              qb_result reason, void *opaque)
{
    USBIPDevice *s = opaque;

    qemu_mutex_lock(&s->lock);
    if (!s->session) {
        s->session = session;
    }
    s->remote_connected = connected;
    s->connection_reason = reason;
    if (!s->closing) {
        qemu_bh_schedule(s->connection_bh);
    }
    qemu_mutex_unlock(&s->lock);
}

static USBIPRequest *usb_ip_request_new(USBIPDevice *s, USBPacket *packet,
                                        size_t length, bool control)
{
    USBIPRequest *r = g_new0(USBIPRequest, 1);

    r->owner = s;
    r->packet = packet;
    r->control = control;
    r->buffer = length ? g_malloc(length) : NULL;
    r->transfer.data = r->buffer;
    r->transfer.length = (uint32_t)length;
    r->transfer.user = r;
    r->bh = qemu_bh_new_guarded(usb_ip_request_bh, r,
                                &DEVICE(s)->mem_reentrancy_guard);
    qemu_mutex_lock(&s->lock);
    QTAILQ_INSERT_TAIL(&s->requests, r, next);
    qemu_mutex_unlock(&s->lock);
    return r;
}

static void usb_ip_submit(USBIPRequest *r, USBPacket *packet)
{
    qb_result result = qb_session_submit(r->owner->session, &r->transfer,
                                         &r->id);
    if (result == QB_OK) {
        packet->status = USB_RET_ASYNC;
        return;
    }
    qemu_mutex_lock(&r->owner->lock);
    QTAILQ_REMOVE(&r->owner->requests, r, next);
    qemu_mutex_unlock(&r->owner->lock);
    packet->status = result == QB_E_DISCONNECTED ? USB_RET_NODEV
                                                  : USB_RET_IOERROR;
    qemu_bh_delete(r->bh);
    g_free(r->buffer);
    g_free(r);
}

static void usb_ip_handle_control(USBDevice *dev, USBPacket *packet,
                                  int request, int value, int index,
                                  int length, uint8_t *data)
{
    USBIPDevice *s = USB_IP_DEVICE(dev);
    USBIPRequest *r;

    if (!s->session || !qb_session_connected(s->session)) {
        packet->status = USB_RET_NODEV;
        return;
    }
    if (request == (DeviceOutRequest | USB_REQ_SET_ADDRESS)) {
        packet->status = USB_RET_SUCCESS;
        return;
    }
    r = usb_ip_request_new(s, packet, length, true);
    r->request = request;
    r->value = value;
    r->index = index;
    r->control_data = data;
    r->transfer.type = QB_TRANSFER_CONTROL;
    r->transfer.direction = (request & USB_DIR_IN) ? QB_DIR_IN : QB_DIR_OUT;
    r->transfer.setup[0] = request >> 8;
    r->transfer.setup[1] = request;
    r->transfer.setup[2] = value;
    r->transfer.setup[3] = value >> 8;
    r->transfer.setup[4] = index;
    r->transfer.setup[5] = index >> 8;
    r->transfer.setup[6] = length;
    r->transfer.setup[7] = length >> 8;
    if (r->transfer.direction == QB_DIR_OUT && length) {
        memcpy(r->buffer, data, length);
    }
    usb_ip_submit(r, packet);
}

static void usb_ip_handle_data(USBDevice *dev, USBPacket *packet)
{
    USBIPDevice *s = USB_IP_DEVICE(dev);
    USBIPRequest *r;
    const size_t length = usb_packet_size(packet);
    const int type = usb_ep_get_type(dev, packet->pid, packet->ep->nr);
    const unsigned dir = packet->pid == USB_TOKEN_IN ? 1 : 0;

    if (!s->session || !qb_session_connected(s->session)) {
        packet->status = USB_RET_NODEV;
        return;
    }
    if (packet->ep->halted) {
        packet->status = USB_RET_STALL;
        return;
    }
    if (type < USB_ENDPOINT_XFER_ISOC || type > USB_ENDPOINT_XFER_INT) {
        packet->status = USB_RET_STALL;
        return;
    }
    r = usb_ip_request_new(s, packet, length, false);
    r->transfer.type = type;
    r->transfer.direction = dir ? QB_DIR_IN : QB_DIR_OUT;
    r->transfer.endpoint = packet->ep->nr;
    r->transfer.interval = s->interval[dir][packet->ep->nr];
    if (packet->short_not_ok) {
        r->transfer.flags |= USBIP_URB_SHORT_NOT_OK;
    }
    if (type == USB_ENDPOINT_XFER_ISOC) {
        r->transfer.flags |= USBIP_URB_ISO_ASAP;
        r->transfer.start_frame = -1;
        r->iso.length = length;
        r->transfer.iso_packets = &r->iso;
        r->transfer.iso_packet_count = 1;
    }
    if (!dir && length) {
        usb_packet_copy(packet, r->buffer, length);
    }
    usb_ip_submit(r, packet);
}

static void usb_ip_cancel_packet(USBDevice *dev, USBPacket *packet)
{
    USBIPDevice *s = USB_IP_DEVICE(dev);
    USBIPRequest *r = NULL;

    qemu_mutex_lock(&s->lock);
    QTAILQ_FOREACH(r, &s->requests, next) {
        if (r->packet == packet) {
            r->packet = NULL;
            break;
        }
    }
    qemu_mutex_unlock(&s->lock);
    if (r && !r->completed) {
        qb_session_cancel(s->session, r->id);
    }
}

static void usb_ip_handle_reset(USBDevice *dev)
{
    USBIPDevice *s = USB_IP_DEVICE(dev);
    USBIPRequest *r;

    qemu_mutex_lock(&s->lock);
    QTAILQ_FOREACH(r, &s->requests, next) {
        if (r->packet) {
            r->packet = NULL;
        }
        if (!r->completed) {
            qb_session_cancel(s->session, r->id);
        }
    }
    qemu_mutex_unlock(&s->lock);
    dev->configuration = 0;
    memset(dev->altsetting, 0, sizeof(dev->altsetting));
    usb_ip_update_endpoints(s);
}

static void usb_ip_realize(USBDevice *dev, Error **errp)
{
    USBIPDevice *s = USB_IP_DEVICE(dev);
    qb_session_config config = { 0 };
    const qb_device_info *info;
    const qb_descriptor_cache *descriptors;
    qb_result result;
    Error *local_err = NULL;

    if (!s->busid || !s->busid[0]) {
        error_setg(errp, "usb-ip requires busid");
        return;
    }
    if (s->vendor_id > UINT16_MAX || s->product_id > UINT16_MAX) {
        error_setg(errp, "usb-ip vendorid/productid must be 16-bit values");
        return;
    }
    config.host = s->host ? s->host : "127.0.0.1";
    config.port = s->port;
    config.busid = s->busid;
    config.connect_timeout_ms = s->connect_timeout_ms;
    config.reconnect_delay_ms = s->reconnect_delay_ms;
    config.auto_reconnect = s->reconnect;
    config.start_disconnected = s->wait;
    config.policy.allow_remote = s->allow_remote;
    config.policy.vendor_id = s->vendor_id;
    config.policy.product_id = s->product_id;
    config.policy.serial = s->serial;
    result = qb_session_open(&config, usb_ip_completion, usb_ip_connection,
                             s, &s->session);
    if (result != QB_OK) {
        error_setg(errp, "usb-ip %s: %s", s->busid,
                   qb_result_string(result));
        return;
    }
    dev->flags |= 1 << USB_DEV_FLAG_IS_HOST;
    dev->auto_attach = 0;
    s->remote_connected = qb_session_connected(s->session);
    if (s->remote_connected) {
        info = qb_session_device(s->session);
        descriptors = qb_session_descriptors(s->session);
        dev->speed = usb_ip_speed(info->speed);
        dev->speedmask = 1 << dev->speed;
        g_strlcpy(dev->product_desc,
                  descriptors->product[0] ? descriptors->product : "USB/IP device",
                  sizeof(dev->product_desc));
        usb_device_attach(dev, &local_err);
        if (local_err) {
            qb_session_close(s->session);
            s->session = NULL;
            error_propagate(errp, local_err);
        }
    }
}

static void usb_ip_unrealize(USBDevice *dev)
{
    USBIPDevice *s = USB_IP_DEVICE(dev);
    USBIPRequest *r, *next;

    qemu_mutex_lock(&s->lock);
    s->closing = true;
    qemu_mutex_unlock(&s->lock);
    if (dev->attached) {
        usb_device_detach(dev);
    }
    qb_session_close(s->session);
    s->session = NULL;
    qemu_bh_delete(s->connection_bh);
    QTAILQ_FOREACH_SAFE(r, &s->requests, next, next) {
        QTAILQ_REMOVE(&s->requests, r, next);
        qemu_bh_delete(r->bh);
        g_free(r->buffer);
        g_free(r);
    }
}

static void usb_ip_instance_init(Object *obj)
{
    USBIPDevice *s = USB_IP_DEVICE(obj);

    qemu_mutex_init(&s->lock);
    QTAILQ_INIT(&s->requests);
    s->connection_bh = qemu_bh_new_guarded(usb_ip_connection_bh, s,
                                            &DEVICE(s)->mem_reentrancy_guard);
}

static void usb_ip_instance_finalize(Object *obj)
{
    USBIPDevice *s = USB_IP_DEVICE(obj);
    qemu_mutex_destroy(&s->lock);
}

static const Property usb_ip_properties[] = {
    DEFINE_PROP_STRING("host", USBIPDevice, host),
    DEFINE_PROP_UINT16("port", USBIPDevice, port, QB_USBIP_DEFAULT_PORT),
    DEFINE_PROP_STRING("busid", USBIPDevice, busid),
    DEFINE_PROP_BOOL("allow-remote", USBIPDevice, allow_remote, false),
    DEFINE_PROP_BOOL("reconnect", USBIPDevice, reconnect, true),
    DEFINE_PROP_BOOL("wait", USBIPDevice, wait, true),
    DEFINE_PROP_UINT32("reconnect-delay", USBIPDevice, reconnect_delay_ms, 1000),
    DEFINE_PROP_UINT32("connect-timeout", USBIPDevice, connect_timeout_ms, 5000),
    DEFINE_PROP_UINT32("vendorid", USBIPDevice, vendor_id, 0),
    DEFINE_PROP_UINT32("productid", USBIPDevice, product_id, 0),
    DEFINE_PROP_STRING("serial", USBIPDevice, serial),
};

static const VMStateDescription vmstate_usb_ip = {
    .name = "usb-ip",
    .unmigratable = 1,
};

static void usb_ip_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->realize = usb_ip_realize;
    uc->unrealize = usb_ip_unrealize;
    uc->product_desc = "USB/IP pass-through device";
    uc->handle_control = usb_ip_handle_control;
    uc->handle_data = usb_ip_handle_data;
    uc->cancel_packet = usb_ip_cancel_packet;
    uc->handle_reset = usb_ip_handle_reset;
    dc->vmsd = &vmstate_usb_ip;
    device_class_set_props(dc, usb_ip_properties);
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
}

static const TypeInfo usb_ip_info = {
    .name = TYPE_USB_IP_DEVICE,
    .parent = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBIPDevice),
    .instance_init = usb_ip_instance_init,
    .instance_finalize = usb_ip_instance_finalize,
    .class_init = usb_ip_class_init,
};

static void usb_ip_register_types(void)
{
    type_register_static(&usb_ip_info);
}

type_init(usb_ip_register_types)
