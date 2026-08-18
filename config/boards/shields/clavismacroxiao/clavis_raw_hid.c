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
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include <zmk/event_manager.h>
#include <raw_hid/events.h>

#include "clavis_rgb_engine.h"

#define RVAN_HID_REPORT_SIZE 32

#define RVAN_MAGIC_0 0x52
#define RVAN_MAGIC_1 0x56
#define RVAN_PROTOCOL_VERSION 0x01

#define RVAN_CMD_PING                0x01
#define RVAN_CMD_GET_DEVICE_INFO     0x02

#define RVAN_CMD_GET_RGB_STATE       0x10
#define RVAN_CMD_SET_RGB_EFFECT      0x11
#define RVAN_CMD_SET_RGB_BRIGHTNESS  0x12
#define RVAN_CMD_SET_RGB_SPEED       0x13

#define RVAN_RSP_PONG                0x81
#define RVAN_RSP_DEVICE_INFO         0x82
#define RVAN_RSP_RGB_STATE           0x90
#define RVAN_RSP_ERROR               0xFE

#define RVAN_DEVICE_CLAVIS_MACRO 0x01

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
#define RVAN_FW_MINOR 2
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

static void rvan_send_error(uint8_t transaction_id,
                            uint8_t command,
                            uint8_t error_code) {
    rvan_prepare_response(
        RVAN_RSP_ERROR,
        transaction_id
    );

    response[5] = command;
    response[6] = error_code;

    rvan_send_response();
}

static void rvan_handle_ping(uint8_t transaction_id) {
    rvan_prepare_response(
        RVAN_RSP_PONG,
        transaction_id
    );

    const char product[] = "CLAVIS";

    memcpy(
        &response[5],
        product,
        sizeof(product) - 1
    );

    rvan_send_response();
}

static void rvan_handle_get_device_info(uint8_t transaction_id) {
    rvan_prepare_response(
        RVAN_RSP_DEVICE_INFO,
        transaction_id
    );

    const uint16_t capabilities =
        RVAN_CLAVIS_CAPABILITIES;

    response[5] = RVAN_DEVICE_CLAVIS_MACRO;
    response[6] = RVAN_HW_REVISION;

    response[7] = RVAN_FW_MAJOR;
    response[8] = RVAN_FW_MINOR;
    response[9] = RVAN_FW_PATCH;

    response[10] =
        (uint8_t)(capabilities & 0xFF);

    response[11] =
        (uint8_t)((capabilities >> 8) & 0xFF);

    const char product_name[] =
        "ClavisMacro";

    memcpy(
        &response[12],
        product_name,
        sizeof(product_name) - 1
    );

    rvan_send_response();
}

static void rvan_send_rgb_state(uint8_t transaction_id) {
    struct clavis_rgb_state rgb;

    if (clavis_rgb_get_state(&rgb) < 0) {
        rvan_send_error(
            transaction_id,
            RVAN_CMD_GET_RGB_STATE,
            1U
        );

        return;
    }

    rvan_prepare_response(
        RVAN_RSP_RGB_STATE,
        transaction_id
    );

    response[5] = rgb.on ? 1U : 0U;
    response[6] = rgb.effect;

    response[7] =
        (uint8_t)(rgb.hue & 0xFF);

    response[8] =
        (uint8_t)((rgb.hue >> 8) & 0xFF);

    response[9] = rgb.saturation;
    response[10] = rgb.brightness;
    response[11] = rgb.speed;
    response[12] = rgb.reverse ? 1U : 0U;

    response[13] =
        (uint8_t)(rgb.led_mask & 0xFF);

    response[14] =
        (uint8_t)((rgb.led_mask >> 8) & 0xFF);

    response[15] = rgb.selected_led;
    response[16] = CLAVIS_RGB_EFFECT_COUNT;

    rvan_send_response();
}

static void rvan_handle_set_rgb_effect(
    const uint8_t *data,
    uint8_t transaction_id
) {
    if (data[5] >= CLAVIS_RGB_EFFECT_COUNT ||
        clavis_rgb_select_effect(data[5]) < 0) {

        rvan_send_error(
            transaction_id,
            RVAN_CMD_SET_RGB_EFFECT,
            1U
        );

        return;
    }

    rvan_send_rgb_state(transaction_id);
}

static void rvan_handle_set_rgb_brightness(
    const uint8_t *data,
    uint8_t transaction_id
) {
    if (data[5] > 100U ||
        clavis_rgb_set_brightness(data[5]) < 0) {

        rvan_send_error(
            transaction_id,
            RVAN_CMD_SET_RGB_BRIGHTNESS,
            1U
        );

        return;
    }

    rvan_send_rgb_state(transaction_id);
}

static void rvan_handle_set_rgb_speed(
    const uint8_t *data,
    uint8_t transaction_id
) {
    if (data[5] < 1U ||
        data[5] > 100U ||
        clavis_rgb_set_speed(data[5]) < 0) {

        rvan_send_error(
            transaction_id,
            RVAN_CMD_SET_RGB_SPEED,
            1U
        );

        return;
    }

    rvan_send_rgb_state(transaction_id);
}

static int clavis_raw_hid_listener(const zmk_event_t *eh) {
    struct raw_hid_received_event *event =
        as_raw_hid_received_event(eh);

    if (event == NULL ||
        event->data == NULL ||
        event->length < 5U) {

        return ZMK_EV_EVENT_BUBBLE;
    }

    const uint8_t *data = event->data;

    if (data[0] != RVAN_MAGIC_0 ||
        data[1] != RVAN_MAGIC_1 ||
        data[2] != RVAN_PROTOCOL_VERSION) {

        return ZMK_EV_EVENT_BUBBLE;
    }

    const uint8_t command =
        data[3];

    const uint8_t transaction_id =
        data[4];

    switch (command) {
    case RVAN_CMD_PING:
        rvan_handle_ping(transaction_id);
        break;

    case RVAN_CMD_GET_DEVICE_INFO:
        rvan_handle_get_device_info(
            transaction_id
        );
        break;

    case RVAN_CMD_GET_RGB_STATE:
        rvan_send_rgb_state(
            transaction_id
        );
        break;

    case RVAN_CMD_SET_RGB_EFFECT:
        rvan_handle_set_rgb_effect(
            data,
            transaction_id
        );
        break;

    case RVAN_CMD_SET_RGB_BRIGHTNESS:
        rvan_handle_set_rgb_brightness(
            data,
            transaction_id
        );
        break;

    case RVAN_CMD_SET_RGB_SPEED:
        rvan_handle_set_rgb_speed(
            data,
            transaction_id
        );
        break;

    default:
        rvan_send_error(
            transaction_id,
            command,
            2U
        );
        break;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(
    clavis_raw_hid,
    clavis_raw_hid_listener
);

ZMK_SUBSCRIPTION(
    clavis_raw_hid,
    raw_hid_received_event
);
