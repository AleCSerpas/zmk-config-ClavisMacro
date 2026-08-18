/*
 * Rvan ClavisMacro - Raw HID configuration protocol
 *
 * Packet format (32 bytes):
 *   byte 0: 0x52 'R'
 *   byte 1: 0x56 'V'
 *   byte 2: protocol version
 *   byte 3: command / response
 *   byte 4: transaction ID
 *   byte 5..31: payload
 */

#include <stdint.h>
#include <string.h>

#include <zmk/event_manager.h>
#include <raw_hid/events.h>

#define RVAN_HID_REPORT_SIZE 32

#define RVAN_MAGIC_0 0x52
#define RVAN_MAGIC_1 0x56
#define RVAN_PROTOCOL_VERSION 0x01

#define RVAN_CMD_PING             0x01
#define RVAN_CMD_GET_DEVICE_INFO  0x02

#define RVAN_RSP_PONG             0x81
#define RVAN_RSP_DEVICE_INFO      0x82

#define RVAN_DEVICE_CLAVIS_MACRO  0x01

#define RVAN_CAP_RGB       (1u << 0)
#define RVAN_CAP_HALL      (1u << 1)
#define RVAN_CAP_DISPLAY   (1u << 2)
#define RVAN_CAP_ENCODERS  (1u << 3)
#define RVAN_CAP_REMAP     (1u << 4)
#define RVAN_CAP_FIRMWARE  (1u << 5)

#define RVAN_CLAVIS_CAPABILITIES \
    (RVAN_CAP_RGB | RVAN_CAP_HALL | RVAN_CAP_DISPLAY | \
     RVAN_CAP_ENCODERS | RVAN_CAP_REMAP | RVAN_CAP_FIRMWARE)

#define RVAN_HW_REVISION 'A'
#define RVAN_FW_MAJOR 0
#define RVAN_FW_MINOR 1
#define RVAN_FW_PATCH 0

static uint8_t response[RVAN_HID_REPORT_SIZE];

static void rvan_prepare_response(uint8_t response_code,
                                  uint8_t transaction_id) {
    memset(response, 0, sizeof(response));

    response[0] = RVAN_MAGIC_0;
    response[1] = RVAN_MAGIC_1;
    response[2] = RVAN_PROTOCOL_VERSION;
    response[3] = response_code;
    response[4] = transaction_id;
}

static void rvan_send_response(void) {
    raise_raw_hid_sent_event(
        (struct raw_hid_sent_event) {
            .data = response,
            .length = sizeof(response),
        }
    );
}

static void rvan_handle_ping(uint8_t transaction_id) {
    rvan_prepare_response(RVAN_RSP_PONG, transaction_id);

    const char product[] = "CLAVIS";
    memcpy(&response[5], product, sizeof(product) - 1);

    rvan_send_response();
}

static void rvan_handle_get_device_info(uint8_t transaction_id) {
    rvan_prepare_response(RVAN_RSP_DEVICE_INFO, transaction_id);

    const uint16_t capabilities = RVAN_CLAVIS_CAPABILITIES;

    response[5] = RVAN_DEVICE_CLAVIS_MACRO;
    response[6] = RVAN_HW_REVISION;
    response[7] = RVAN_FW_MAJOR;
    response[8] = RVAN_FW_MINOR;
    response[9] = RVAN_FW_PATCH;
    response[10] = (uint8_t)(capabilities & 0xFF);
    response[11] = (uint8_t)((capabilities >> 8) & 0xFF);

    const char product_name[] = "ClavisMacro";
    memcpy(&response[12], product_name, sizeof(product_name) - 1);

    rvan_send_response();
}

static int clavis_raw_hid_listener(const zmk_event_t *eh) {
    struct raw_hid_received_event *event =
        as_raw_hid_received_event(eh);

    if (event == NULL || event->data == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (event->length < 5) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    const uint8_t *data = event->data;

    if (data[0] != RVAN_MAGIC_0 ||
        data[1] != RVAN_MAGIC_1 ||
        data[2] != RVAN_PROTOCOL_VERSION) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    const uint8_t command = data[3];
    const uint8_t transaction_id = data[4];

    switch (command) {
    case RVAN_CMD_PING:
        rvan_handle_ping(transaction_id);
        break;

    case RVAN_CMD_GET_DEVICE_INFO:
        rvan_handle_get_device_info(transaction_id);
        break;

    default:
        break;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(clavis_raw_hid, clavis_raw_hid_listener);
ZMK_SUBSCRIPTION(clavis_raw_hid, raw_hid_received_event);
