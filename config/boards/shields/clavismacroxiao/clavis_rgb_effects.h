/*
 * ClavisMacro QMK-style RGB effect renderer.
 *
 * The effect names/behavior categories mirror the QMK RGB Matrix catalog,
 * while the implementation is native to the Clavis engine and its 10-LED map.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/led_strip.h>

#include "clavis_rgb_engine.h"

bool clavis_rgb_effect_is_animated(uint8_t effect);

void clavis_rgb_effect_render(const struct clavis_rgb_state *state,
                              uint32_t phase,
                              struct led_rgb pixels[CLAVIS_RGB_LED_COUNT]);

void clavis_rgb_effect_note_key_event(uint8_t key_id, bool pressed);
