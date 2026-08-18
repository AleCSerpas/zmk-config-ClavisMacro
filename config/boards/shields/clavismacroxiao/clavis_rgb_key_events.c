/*
 * Feed ZMK physical key position events into Clavis RGB reactive effects.
 *
 * Hall keys occupy positions 0..8 in the ClavisMacro keymap.
 *
 * SPDX-License-Identifier: MIT
 */

#include "clavis_rgb_engine.h"

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#define CLAVIS_RGB_REACTIVE_KEY_COUNT 9U

static int clavis_rgb_position_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *event =
        as_zmk_position_state_changed(eh);

    if (event == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (event->position < CLAVIS_RGB_REACTIVE_KEY_COUNT) {
        clavis_rgb_note_key_event(
            (uint8_t)event->position,
            event->state
        );
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(
    clavis_rgb_position_events,
    clavis_rgb_position_listener
);

ZMK_SUBSCRIPTION(
    clavis_rgb_position_events,
    zmk_position_state_changed
);
