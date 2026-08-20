/*
 * Rvan ClavisMacro - Runtime screen configuration
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#define CLAVIS_SCREEN_SHOW_LAYER   (1U << 0)
#define CLAVIS_SCREEN_SHOW_RGB     (1U << 1)
#define CLAVIS_SCREEN_SHOW_BATTERY (1U << 2)
#define CLAVIS_SCREEN_SHOW_OUTPUT  (1U << 3)
#define CLAVIS_SCREEN_SHOW_VOLUME  (1U << 4)

#define CLAVIS_SCREEN_ALLOWED_FLAGS \
    (CLAVIS_SCREEN_SHOW_LAYER | CLAVIS_SCREEN_SHOW_RGB | \
     CLAVIS_SCREEN_SHOW_BATTERY | CLAVIS_SCREEN_SHOW_OUTPUT | \
     CLAVIS_SCREEN_SHOW_VOLUME)

#define CLAVIS_SCREEN_DEFAULT_FLAGS CLAVIS_SCREEN_ALLOWED_FLAGS

struct clavis_screen_state {
    uint8_t flags;
};

int clavis_screen_get_state(struct clavis_screen_state *out_state);
int clavis_screen_set_flags(uint8_t flags);
