/*
 * ClavisMacro RGB Matrix engine public API
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "clavis_led_map.h"

enum clavis_rgb_effect {
    CLAVIS_RGB_EFFECT_STATIC = 0,
    CLAVIS_RGB_EFFECT_BREATHE,
    CLAVIS_RGB_EFFECT_WAVE_X,
    CLAVIS_RGB_EFFECT_WAVE_Y,
    CLAVIS_RGB_EFFECT_WAVE_DIAGONAL,
    CLAVIS_RGB_EFFECT_RAINBOW_SPATIAL,
    CLAVIS_RGB_EFFECT_CHASE,
    CLAVIS_RGB_EFFECT_INDEX_SCAN,
    CLAVIS_RGB_EFFECT_COUNT,
};

struct clavis_rgb_state {
    bool on;
    uint8_t effect;
    uint16_t hue;       /* 0..359 */
    uint8_t saturation; /* 0..100 */
    uint8_t brightness; /* 0..100 */
    uint8_t speed;      /* 1..10 */
    bool reverse;
    uint16_t led_mask;  /* bits 0..9 */
    uint8_t selected_led;
};

int clavis_rgb_on(void);
int clavis_rgb_off(void);
int clavis_rgb_toggle(void);

int clavis_rgb_select_effect(uint8_t effect);
int clavis_rgb_cycle_effect(int direction);

int clavis_rgb_set_hsb(uint16_t hue, uint8_t saturation, uint8_t brightness);
int clavis_rgb_change_hue(int direction);
int clavis_rgb_change_saturation(int direction);
int clavis_rgb_change_brightness(int direction);
int clavis_rgb_change_speed(int direction);

int clavis_rgb_set_reverse(bool reverse);
int clavis_rgb_toggle_reverse(void);

int clavis_rgb_set_led_mask(uint16_t mask);
int clavis_rgb_set_selected_led(uint8_t led_index);
int clavis_rgb_select_next_led(int direction);

int clavis_rgb_get_state(struct clavis_rgb_state *out_state);
const char *clavis_rgb_effect_name(uint8_t effect);
