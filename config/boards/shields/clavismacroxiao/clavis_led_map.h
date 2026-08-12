/*
 * ClavisMacro RGB Matrix — physical LED map
 *
 * SPDX-License-Identifier: MIT
 *
 * Electrical LED indices 0..8 are assumed to match the nine Hall keys in
 * row-major order. LED 9 is the encoder/auxiliary LED. Coordinates are
 * normalized to a 0..255 canvas so effects are independent of PCB dimensions.
 *
 * REV 2 does not require changes here unless the daisy-chain order changes.
 */

#pragma once

#include <stdint.h>

#define CLAVIS_RGB_LED_COUNT 10U
#define CLAVIS_RGB_ALL_LEDS_MASK ((uint16_t)((1U << CLAVIS_RGB_LED_COUNT) - 1U))

#define CLAVIS_LED_NO_KEY 0xFFU

enum clavis_led_zone {
    CLAVIS_LED_ZONE_KEYS = 0,
    CLAVIS_LED_ZONE_ENCODER = 1,
};

struct clavis_led_position {
    uint8_t x;
    uint8_t y;
    uint8_t key_id;
    uint8_t zone;
};

/*
 * Current provisional physical layout:
 *
 *     0       1       2
 *
 *     3       4       5        9 (encoder/aux)
 *
 *     6       7       8
 *
 * Change only these coordinates if the real PCB placement differs.
 */
static const struct clavis_led_position clavis_led_map[CLAVIS_RGB_LED_COUNT] = {
    [0] = {.x = 32,  .y = 32,  .key_id = 0,                 .zone = CLAVIS_LED_ZONE_KEYS},
    [1] = {.x = 112, .y = 32,  .key_id = 1,                 .zone = CLAVIS_LED_ZONE_KEYS},
    [2] = {.x = 192, .y = 32,  .key_id = 2,                 .zone = CLAVIS_LED_ZONE_KEYS},

    [3] = {.x = 192, .y = 112, .key_id = 5,                 .zone = CLAVIS_LED_ZONE_KEYS},
    [4] = {.x = 112, .y = 112, .key_id = 4,                 .zone = CLAVIS_LED_ZONE_KEYS},
    [5] = {.x = 32,  .y = 112, .key_id = 3,                 .zone = CLAVIS_LED_ZONE_KEYS},

    [6] = {.x = 32,  .y = 192, .key_id = 6,                 .zone = CLAVIS_LED_ZONE_KEYS},
    [7] = {.x = 112, .y = 192, .key_id = 7,                 .zone = CLAVIS_LED_ZONE_KEYS},
    [8] = {.x = 192, .y = 192, .key_id = 8,                 .zone = CLAVIS_LED_ZONE_KEYS},

    [9] = {.x = 244, .y = 112, .key_id = CLAVIS_LED_NO_KEY, .zone = CLAVIS_LED_ZONE_ENCODER},
};
