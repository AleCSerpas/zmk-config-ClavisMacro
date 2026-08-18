/*
 * Rvan ClavisMacro - Raw HID protocol test
 *
 * Milestone 2:
 * Browser sends PING, Clavis replies PONG.
 *
 * Packet format (32 bytes):
 *   byte 0: 0x52 'R'
 *   byte 1: 0x56 'V'
 *   byte 2: protocol version
 *   byte 3: command
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

#define RVAN_CMD_PING 0x01
#define RVAN_RSP_PONG 0x81

static uint8_t response[RVAN_HID_REPORT_SIZE];

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

    /* Ignore packets that are not using the Rvan protocol. */
    if (data[0] != RVAN_MAGIC_0 ||
        data[1] != RVAN_MAGIC_1 ||
        data[2] != RVAN_PROTOCOL_VERSION) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint8_t command = data[3];
    uint8_t transaction_id = data[4];

    if (command == RVAN_CMD_PING) {
        memset(response, 0, sizeof(response));

        response[0] = RVAN_MAGIC_0;
        response[1] = RVAN_MAGIC_1;
        response[2] = RVAN_PROTOCOL_VERSION;
        response[3] = RVAN_RSP_PONG;
        response[4] = transaction_id;

        /* Human-readable payload for the first test. */
        const char product[] = "CLAVIS";
        memcpy(&response[5], product, sizeof(product) - 1);

        raise_raw_hid_sent_event(
            (struct raw_hid_sent_event) {
                .data = response,
                .length = sizeof(response),
            }
        );
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(clavis_raw_hid, clavis_raw_hid_listener);
ZMK_SUBSCRIPTION(clavis_raw_hid, raw_hid_received_event);