/*
 * ClavisMacro RGB Matrix engine public API
 *
 * QMK-style effect catalog adapted for the Rvan ClavisMacro.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "clavis_led_map.h"

enum clavis_rgb_effect {
    CLAVIS_RGB_EFFECT_SOLID_COLOR = 0,
    CLAVIS_RGB_EFFECT_ALPHAS_MODS,
    CLAVIS_RGB_EFFECT_GRADIENT_UP_DOWN,
    CLAVIS_RGB_EFFECT_GRADIENT_LEFT_RIGHT,
    CLAVIS_RGB_EFFECT_BREATHING,
    CLAVIS_RGB_EFFECT_BAND_SAT,
    CLAVIS_RGB_EFFECT_BAND_VAL,
    CLAVIS_RGB_EFFECT_BAND_PINWHEEL_SAT,
    CLAVIS_RGB_EFFECT_BAND_PINWHEEL_VAL,
    CLAVIS_RGB_EFFECT_BAND_SPIRAL_SAT,
    CLAVIS_RGB_EFFECT_BAND_SPIRAL_VAL,
    CLAVIS_RGB_EFFECT_CYCLE_ALL,
    CLAVIS_RGB_EFFECT_CYCLE_LEFT_RIGHT,
    CLAVIS_RGB_EFFECT_CYCLE_UP_DOWN,
    CLAVIS_RGB_EFFECT_CYCLE_OUT_IN,
    CLAVIS_RGB_EFFECT_CYCLE_OUT_IN_DUAL,
    CLAVIS_RGB_EFFECT_RAINBOW_MOVING_CHEVRON,
    CLAVIS_RGB_EFFECT_CYCLE_PINWHEEL,
    CLAVIS_RGB_EFFECT_CYCLE_SPIRAL,
    CLAVIS_RGB_EFFECT_DUAL_BEACON,
    CLAVIS_RGB_EFFECT_RAINBOW_BEACON,
    CLAVIS_RGB_EFFECT_RAINBOW_PINWHEELS,
    CLAVIS_RGB_EFFECT_FLOWER_BLOOMING,
    CLAVIS_RGB_EFFECT_RAINDROPS,
    CLAVIS_RGB_EFFECT_JELLYBEAN_RAINDROPS,
    CLAVIS_RGB_EFFECT_HUE_BREATHING,
    CLAVIS_RGB_EFFECT_HUE_PENDULUM,
    CLAVIS_RGB_EFFECT_HUE_WAVE,
    CLAVIS_RGB_EFFECT_PIXEL_FRACTAL,
    CLAVIS_RGB_EFFECT_PIXEL_FLOW,
    CLAVIS_RGB_EFFECT_PIXEL_RAIN,
    CLAVIS_RGB_EFFECT_TYPING_HEATMAP,
    CLAVIS_RGB_EFFECT_DIGITAL_RAIN,
    CLAVIS_RGB_EFFECT_SOLID_REACTIVE_SIMPLE,
    CLAVIS_RGB_EFFECT_SOLID_REACTIVE,
    CLAVIS_RGB_EFFECT_SOLID_REACTIVE_WIDE,
    CLAVIS_RGB_EFFECT_SOLID_REACTIVE_MULTIWIDE,
    CLAVIS_RGB_EFFECT_SOLID_REACTIVE_CROSS,
    CLAVIS_RGB_EFFECT_SOLID_REACTIVE_MULTICROSS,
    CLAVIS_RGB_EFFECT_SOLID_REACTIVE_NEXUS,
    CLAVIS_RGB_EFFECT_SOLID_REACTIVE_MULTINEXUS,
    CLAVIS_RGB_EFFECT_SPLASH,
    CLAVIS_RGB_EFFECT_MULTISPLASH,
    CLAVIS_RGB_EFFECT_SOLID_SPLASH,
    CLAVIS_RGB_EFFECT_SOLID_MULTISPLASH,
    CLAVIS_RGB_EFFECT_STARLIGHT,
    CLAVIS_RGB_EFFECT_STARLIGHT_SMOOTH,
    CLAVIS_RGB_EFFECT_STARLIGHT_DUAL_HUE,
    CLAVIS_RGB_EFFECT_STARLIGHT_DUAL_SAT,
    CLAVIS_RGB_EFFECT_RIVERFLOW,
    CLAVIS_RGB_EFFECT_COUNT,
};

#define CLAVIS_RGB_QMK_EFFECT_COUNT CLAVIS_RGB_EFFECT_COUNT

struct clavis_rgb_state {
    bool on;
    uint8_t effect;
    uint16_t hue;       /* 0..359 */
    uint8_t saturation; /* 0..100 */
    uint8_t brightness; /* 0..100 */
    uint8_t speed;      /* 1..100 */
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
int clavis_rgb_set_brightness(uint8_t brightness);
int clavis_rgb_set_speed(uint8_t speed);

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

/* Feed physical key activity to the reactive/heatmap effects. */
void clavis_rgb_note_key_event(uint8_t key_id, bool pressed);
