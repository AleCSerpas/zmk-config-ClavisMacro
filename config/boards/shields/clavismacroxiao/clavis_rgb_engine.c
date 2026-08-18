/*
 * ClavisMacro RGB Matrix engine
 *
 * Uses Zephyr's LED strip API directly. Rendering is delegated to the
 * Clavis QMK-style effect catalog in clavis_rgb_effects.c.
 *
 * SPDX-License-Identifier: MIT
 */

#include "clavis_rgb_engine.h"
#include "clavis_rgb_effects.h"

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(clavis_rgb, LOG_LEVEL_INF);

#if !DT_HAS_CHOSEN(zmk_underglow)
#error "Clavis RGB requires a chosen { zmk,underglow = &strip; }; node"
#endif

#define STRIP_NODE DT_CHOSEN(zmk_underglow)
#define STRIP_LED_COUNT DT_PROP(STRIP_NODE, chain_length)

BUILD_ASSERT(STRIP_LED_COUNT == CLAVIS_RGB_LED_COUNT,
             "The LED strip chain-length must match CLAVIS_RGB_LED_COUNT");

#define CLAVIS_RGB_FRAME_MS 25
#define CLAVIS_RGB_STATIC_POLL_MS 250
#define CLAVIS_RGB_OFF_POLL_MS 500

#define CLAVIS_RGB_HUE_STEP 15
#define CLAVIS_RGB_SAT_STEP 10
#define CLAVIS_RGB_BRIGHTNESS_STEP 5
#define CLAVIS_RGB_SPEED_STEP 5

#define CLAVIS_RGB_SETTINGS_MAGIC 0x43524742U /* 'CRGB' */
#define CLAVIS_RGB_SETTINGS_VERSION 2U
#define CLAVIS_RGB_PHASE_MODULUS 65536U

#ifndef CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE
#define CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE 500
#endif

struct clavis_rgb_persisted_state {
    uint32_t magic;
    uint8_t version;
    uint8_t on;
    uint8_t effect;
    uint16_t hue;
    uint8_t saturation;
    uint8_t brightness;
    uint8_t speed;
    uint8_t reverse;
    uint16_t led_mask;
    uint8_t selected_led;
};

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb pixels[CLAVIS_RGB_LED_COUNT];

static struct clavis_rgb_state state = {
    .on = true,
    .effect = CLAVIS_RGB_EFFECT_CYCLE_LEFT_RIGHT,
    .hue = 190,
    .saturation = 100,
    .brightness = 35,
    .speed = 30,
    .reverse = false,
    .led_mask = CLAVIS_RGB_ALL_LEDS_MASK,
    .selected_led = 0,
};

static struct k_mutex state_lock;
static struct k_work_delayable render_work;

#if IS_ENABLED(CONFIG_SETTINGS)
static struct k_work_delayable save_work;
#endif

static bool engine_ready;
static bool frame_dirty = true;
static uint32_t animation_phase;
static bool strip_was_cleared;

static void clear_pixels(void) {
    memset(pixels, 0, sizeof(pixels));
}

static void request_render(void) {
    if (engine_ready) {
        k_work_reschedule(&render_work, K_NO_WAIT);
    }
}

#if IS_ENABLED(CONFIG_SETTINGS)
static void schedule_save(void) {
    if (engine_ready) {
        k_work_reschedule(&save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
    }
}
#else
static void schedule_save(void) {}
#endif

static void mark_changed(bool reset_phase) {
    k_mutex_lock(&state_lock, K_FOREVER);

    frame_dirty = true;
    strip_was_cleared = false;

    if (reset_phase) {
        animation_phase = 0U;
    }

    k_mutex_unlock(&state_lock);

    request_render();
    schedule_save();
}

static uint32_t phase_step_for_speed(uint8_t speed) {
    return 1U + (MIN(speed, 100U) / 18U);
}

static void render_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    struct clavis_rgb_state snapshot;
    uint32_t phase;
    bool dirty;
    bool was_cleared;

    k_mutex_lock(&state_lock, K_FOREVER);

    snapshot = state;
    phase = animation_phase;
    dirty = frame_dirty;
    was_cleared = strip_was_cleared;

    if (snapshot.on && clavis_rgb_effect_is_animated(snapshot.effect)) {
        uint32_t step = phase_step_for_speed(snapshot.speed);

        if (snapshot.reverse) {
            animation_phase =
                (animation_phase + CLAVIS_RGB_PHASE_MODULUS - step) %
                CLAVIS_RGB_PHASE_MODULUS;
        } else {
            animation_phase =
                (animation_phase + step) %
                CLAVIS_RGB_PHASE_MODULUS;
        }

        dirty = true;
    }

    k_mutex_unlock(&state_lock);

    int next_delay_ms = CLAVIS_RGB_STATIC_POLL_MS;
    int err = 0;

    if (!snapshot.on) {
        next_delay_ms = CLAVIS_RGB_OFF_POLL_MS;

        if (!was_cleared || dirty) {
            clear_pixels();

            err = led_strip_update_rgb(
                strip,
                pixels,
                CLAVIS_RGB_LED_COUNT
            );

            if (err == 0) {
                k_mutex_lock(&state_lock, K_FOREVER);
                frame_dirty = false;
                strip_was_cleared = true;
                k_mutex_unlock(&state_lock);
            }
        }

    } else if (dirty || clavis_rgb_effect_is_animated(snapshot.effect)) {
        clavis_rgb_effect_render(&snapshot, phase, pixels);

        err = led_strip_update_rgb(
            strip,
            pixels,
            CLAVIS_RGB_LED_COUNT
        );

        if (err == 0) {
            k_mutex_lock(&state_lock, K_FOREVER);
            frame_dirty = false;
            strip_was_cleared = false;
            k_mutex_unlock(&state_lock);
        }

        next_delay_ms =
            clavis_rgb_effect_is_animated(snapshot.effect)
                ? CLAVIS_RGB_FRAME_MS
                : CLAVIS_RGB_STATIC_POLL_MS;
    }

    if (err < 0) {
        LOG_ERR("led_strip_update_rgb failed: %d", err);
        next_delay_ms = 100;
    }

    k_work_schedule(
        &render_work,
        K_MSEC(next_delay_ms)
    );
}

#if IS_ENABLED(CONFIG_SETTINGS)
static void save_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    struct clavis_rgb_state snapshot;

    k_mutex_lock(&state_lock, K_FOREVER);
    snapshot = state;
    k_mutex_unlock(&state_lock);

    const struct clavis_rgb_persisted_state persisted = {
        .magic = CLAVIS_RGB_SETTINGS_MAGIC,
        .version = CLAVIS_RGB_SETTINGS_VERSION,
        .on = snapshot.on,
        .effect = snapshot.effect,
        .hue = snapshot.hue,
        .saturation = snapshot.saturation,
        .brightness = snapshot.brightness,
        .speed = snapshot.speed,
        .reverse = snapshot.reverse,
        .led_mask = snapshot.led_mask,
        .selected_led = snapshot.selected_led,
    };

    int err = settings_save_one(
        "clavis/rgb/state",
        &persisted,
        sizeof(persisted)
    );

    if (err) {
        LOG_ERR("Unable to save RGB state: %d", err);
    }
}

static int settings_set(const char *name,
                        size_t len,
                        settings_read_cb read_cb,
                        void *cb_arg) {
    const char *next;

    if (!settings_name_steq(name, "state", &next) || next != NULL) {
        return -ENOENT;
    }

    if (len != sizeof(struct clavis_rgb_persisted_state)) {
        return -EINVAL;
    }

    struct clavis_rgb_persisted_state persisted;

    int rc = read_cb(
        cb_arg,
        &persisted,
        sizeof(persisted)
    );

    if (rc < 0) {
        return rc;
    }

    if (persisted.magic != CLAVIS_RGB_SETTINGS_MAGIC ||
        persisted.version != CLAVIS_RGB_SETTINGS_VERSION ||
        persisted.effect >= CLAVIS_RGB_EFFECT_COUNT ||
        persisted.hue >= 360U ||
        persisted.saturation > 100U ||
        persisted.brightness > 100U ||
        persisted.speed < 1U ||
        persisted.speed > 100U ||
        persisted.selected_led >= CLAVIS_RGB_LED_COUNT) {

        LOG_WRN("Ignoring incompatible Clavis RGB settings");
        return -EINVAL;
    }

    struct clavis_rgb_state loaded = {
        .on = persisted.on != 0U,
        .effect = persisted.effect,
        .hue = persisted.hue,
        .saturation = persisted.saturation,
        .brightness = persisted.brightness,
        .speed = persisted.speed,
        .reverse = persisted.reverse != 0U,
        .led_mask = persisted.led_mask & CLAVIS_RGB_ALL_LEDS_MASK,
        .selected_led = persisted.selected_led,
    };

    if (engine_ready) {
        k_mutex_lock(&state_lock, K_FOREVER);
        state = loaded;
        frame_dirty = true;
        strip_was_cleared = false;
        animation_phase = 0U;
        k_mutex_unlock(&state_lock);

        request_render();

    } else {
        state = loaded;
        frame_dirty = true;
        strip_was_cleared = false;
        animation_phase = 0U;
    }

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(
    clavis_rgb_settings,
    "clavis/rgb",
    NULL,
    settings_set,
    NULL,
    NULL
);
#endif

static int clavis_rgb_init(void) {
    if (!device_is_ready(strip)) {
        LOG_ERR("RGB strip device is not ready");
        return -ENODEV;
    }

    k_mutex_init(&state_lock);
    k_work_init_delayable(&render_work, render_work_handler);

#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_init_delayable(&save_work, save_work_handler);
#endif

    engine_ready = true;

    k_work_schedule(
        &render_work,
        K_MSEC(250)
    );

    LOG_INF(
        "Clavis RGB Matrix ready: %u LEDs, %u effects",
        CLAVIS_RGB_LED_COUNT,
        CLAVIS_RGB_EFFECT_COUNT
    );

    return 0;
}

SYS_INIT(clavis_rgb_init, APPLICATION, 90);

int clavis_rgb_on(void) {
    k_mutex_lock(&state_lock, K_FOREVER);
    state.on = true;
    k_mutex_unlock(&state_lock);

    mark_changed(true);
    return 0;
}

int clavis_rgb_off(void) {
    k_mutex_lock(&state_lock, K_FOREVER);
    state.on = false;
    k_mutex_unlock(&state_lock);

    mark_changed(false);
    return 0;
}

int clavis_rgb_toggle(void) {
    k_mutex_lock(&state_lock, K_FOREVER);
    state.on = !state.on;
    k_mutex_unlock(&state_lock);

    mark_changed(true);
    return 0;
}

int clavis_rgb_select_effect(uint8_t effect) {
    if (effect >= CLAVIS_RGB_EFFECT_COUNT) {
        return -EINVAL;
    }

    k_mutex_lock(&state_lock, K_FOREVER);
    state.effect = effect;
    state.on = true;
    k_mutex_unlock(&state_lock);

    mark_changed(true);
    return 0;
}

int clavis_rgb_cycle_effect(int direction) {
    if (direction == 0) {
        return 0;
    }

    k_mutex_lock(&state_lock, K_FOREVER);

    int effect =
        (int)state.effect +
        (direction > 0 ? 1 : -1);

    if (effect < 0) {
        effect = CLAVIS_RGB_EFFECT_COUNT - 1;

    } else if (effect >= CLAVIS_RGB_EFFECT_COUNT) {
        effect = 0;
    }

    state.effect = (uint8_t)effect;
    state.on = true;

    k_mutex_unlock(&state_lock);

    mark_changed(true);
    return 0;
}

int clavis_rgb_set_hsb(uint16_t hue,
                       uint8_t saturation,
                       uint8_t brightness) {
    if (hue >= 360U ||
        saturation > 100U ||
        brightness > 100U) {
        return -EINVAL;
    }

    k_mutex_lock(&state_lock, K_FOREVER);

    state.hue = hue;
    state.saturation = saturation;
    state.brightness = brightness;
    state.on = true;

    k_mutex_unlock(&state_lock);

    mark_changed(false);
    return 0;
}

int clavis_rgb_set_brightness(uint8_t brightness) {
    if (brightness > 100U) {
        return -EINVAL;
    }

    k_mutex_lock(&state_lock, K_FOREVER);
    state.brightness = brightness;
    state.on = true;
    k_mutex_unlock(&state_lock);

    mark_changed(false);
    return 0;
}

int clavis_rgb_set_speed(uint8_t speed) {
    if (speed < 1U || speed > 100U) {
        return -EINVAL;
    }

    k_mutex_lock(&state_lock, K_FOREVER);
    state.speed = speed;
    k_mutex_unlock(&state_lock);

    mark_changed(false);
    return 0;
}

int clavis_rgb_change_hue(int direction) {
    k_mutex_lock(&state_lock, K_FOREVER);

    int hue =
        (int)state.hue +
        (direction * CLAVIS_RGB_HUE_STEP);

    while (hue < 0) {
        hue += 360;
    }

    state.hue = (uint16_t)(hue % 360);
    state.on = true;

    k_mutex_unlock(&state_lock);

    mark_changed(false);
    return 0;
}

int clavis_rgb_change_saturation(int direction) {
    k_mutex_lock(&state_lock, K_FOREVER);

    int saturation =
        (int)state.saturation +
        (direction * CLAVIS_RGB_SAT_STEP);

    state.saturation =
        (uint8_t)CLAMP(saturation, 0, 100);

    state.on = true;

    k_mutex_unlock(&state_lock);

    mark_changed(false);
    return 0;
}

int clavis_rgb_change_brightness(int direction) {
    k_mutex_lock(&state_lock, K_FOREVER);

    int brightness =
        (int)state.brightness +
        (direction * CLAVIS_RGB_BRIGHTNESS_STEP);

    state.brightness =
        (uint8_t)CLAMP(brightness, 0, 100);

    state.on = true;

    k_mutex_unlock(&state_lock);

    mark_changed(false);
    return 0;
}

int clavis_rgb_change_speed(int direction) {
    k_mutex_lock(&state_lock, K_FOREVER);

    int speed =
        (int)state.speed +
        (direction * CLAVIS_RGB_SPEED_STEP);

    state.speed =
        (uint8_t)CLAMP(speed, 1, 100);

    k_mutex_unlock(&state_lock);

    mark_changed(false);
    return 0;
}

int clavis_rgb_set_reverse(bool reverse) {
    k_mutex_lock(&state_lock, K_FOREVER);
    state.reverse = reverse;
    k_mutex_unlock(&state_lock);

    mark_changed(false);
    return 0;
}

int clavis_rgb_toggle_reverse(void) {
    k_mutex_lock(&state_lock, K_FOREVER);
    state.reverse = !state.reverse;
    k_mutex_unlock(&state_lock);

    mark_changed(false);
    return 0;
}

int clavis_rgb_set_led_mask(uint16_t mask) {
    k_mutex_lock(&state_lock, K_FOREVER);
    state.led_mask = mask & CLAVIS_RGB_ALL_LEDS_MASK;
    k_mutex_unlock(&state_lock);

    mark_changed(false);
    return 0;
}

int clavis_rgb_set_selected_led(uint8_t led_index) {
    if (led_index >= CLAVIS_RGB_LED_COUNT) {
        return -EINVAL;
    }

    k_mutex_lock(&state_lock, K_FOREVER);
    state.selected_led = led_index;
    k_mutex_unlock(&state_lock);

    mark_changed(false);
    return 0;
}

int clavis_rgb_select_next_led(int direction) {
    k_mutex_lock(&state_lock, K_FOREVER);

    int selected =
        (int)state.selected_led +
        (direction >= 0 ? 1 : -1);

    if (selected < 0) {
        selected = CLAVIS_RGB_LED_COUNT - 1;

    } else if (selected >= CLAVIS_RGB_LED_COUNT) {
        selected = 0;
    }

    state.selected_led = (uint8_t)selected;

    k_mutex_unlock(&state_lock);

    mark_changed(false);
    return 0;
}

int clavis_rgb_get_state(struct clavis_rgb_state *out_state) {
    if (out_state == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&state_lock, K_FOREVER);
    *out_state = state;
    k_mutex_unlock(&state_lock);

    return 0;
}

void clavis_rgb_note_key_event(uint8_t key_id, bool pressed) {
    clavis_rgb_effect_note_key_event(key_id, pressed);

    if (pressed) {
        request_render();
    }
}
