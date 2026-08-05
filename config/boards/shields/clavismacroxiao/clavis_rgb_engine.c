/*
 * ClavisMacro RGB Matrix engine
 *
 * Uses Zephyr's LED strip API directly. It intentionally does not use ZMK's
 * built-in rgb_underglow worker because the direct delayable-work test is the
 * implementation that updates this board reliably.
 *
 * SPDX-License-Identifier: MIT
 */

#include "clavis_rgb_engine.h"

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
#define CLAVIS_RGB_BRIGHTNESS_STEP 10
#define CLAVIS_RGB_SPEED_STEP 1

#define CLAVIS_RGB_SETTINGS_MAGIC 0x43524742U /* 'CRGB' */
#define CLAVIS_RGB_SETTINGS_VERSION 1U
#define CLAVIS_RGB_PHASE_MODULUS 23040U /* LCM(256, 360) */

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
    .effect = CLAVIS_RGB_EFFECT_WAVE_X,
    .hue = 190,
    .saturation = 100,
    .brightness = 35,
    .speed = 3,
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

static uint8_t percent_to_u8(uint8_t percent) {
    return (uint8_t)(((uint16_t)MIN(percent, 100U) * 255U) / 100U);
}

static struct led_rgb hsv_to_rgb(uint16_t hue, uint8_t saturation, uint8_t value) {
    hue %= 360U;

    const uint8_t s = percent_to_u8(saturation);
    const uint8_t v = percent_to_u8(value);

    if (s == 0U) {
        return (struct led_rgb){.r = v, .g = v, .b = v};
    }

    const uint8_t region = (uint8_t)(hue / 60U);
    const uint8_t remainder = (uint8_t)(((hue % 60U) * 255U) / 60U);

    const uint8_t p = (uint8_t)(((uint16_t)v * (255U - s)) / 255U);
    const uint8_t q = (uint8_t)(((uint16_t)v *
                                 (255U - (((uint16_t)s * remainder) / 255U))) /
                                255U);
    const uint8_t t = (uint8_t)(((uint16_t)v *
                                 (255U - (((uint16_t)s * (255U - remainder)) / 255U))) /
                                255U);

    switch (region) {
    case 0:
        return (struct led_rgb){.r = v, .g = t, .b = p};
    case 1:
        return (struct led_rgb){.r = q, .g = v, .b = p};
    case 2:
        return (struct led_rgb){.r = p, .g = v, .b = t};
    case 3:
        return (struct led_rgb){.r = p, .g = q, .b = v};
    case 4:
        return (struct led_rgb){.r = t, .g = p, .b = v};
    default:
        return (struct led_rgb){.r = v, .g = p, .b = q};
    }
}

/* 0..255 phase -> smooth 0..255 pulse without floating point. */
static uint8_t smooth_wave8(uint8_t phase) {
    uint16_t triangle = phase < 128U ? (uint16_t)phase * 2U
                                     : (uint16_t)(255U - phase) * 2U;

    /* Smoothstep: x*x*(3-2x), with x normalized to 0..255. */
    uint32_t x = triangle;
    uint32_t smooth = x * x * (765U - (2U * x));
    smooth /= (255U * 255U);

    return (uint8_t)MIN(smooth, 255U);
}

static uint8_t scale_brightness(uint8_t brightness, uint8_t multiplier) {
    return (uint8_t)(((uint16_t)brightness * multiplier) / 255U);
}

static bool led_selected(const struct clavis_rgb_state *snapshot, uint8_t index) {
    return (snapshot->led_mask & BIT(index)) != 0U;
}

static void clear_pixels(void) {
    memset(pixels, 0, sizeof(pixels));
}

static void set_pixel_hsv(uint8_t index, uint16_t hue, uint8_t saturation,
                          uint8_t brightness) {
    if (index >= CLAVIS_RGB_LED_COUNT) {
        return;
    }

    pixels[index] = hsv_to_rgb(hue, saturation, brightness);
}

static void render_static(const struct clavis_rgb_state *snapshot) {
    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (led_selected(snapshot, i)) {
            set_pixel_hsv(i, snapshot->hue, snapshot->saturation, snapshot->brightness);
        }
    }
}

static void render_breathe(const struct clavis_rgb_state *snapshot, uint32_t phase) {
    uint8_t pulse = smooth_wave8((uint8_t)phase);
    uint8_t brightness = scale_brightness(snapshot->brightness, pulse);

    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (led_selected(snapshot, i)) {
            set_pixel_hsv(i, snapshot->hue, snapshot->saturation, brightness);
        }
    }
}

static void render_wave_axis(const struct clavis_rgb_state *snapshot, uint32_t phase,
                             bool use_x, bool use_y) {
    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (!led_selected(snapshot, i)) {
            continue;
        }

        uint16_t spatial = 0U;
        if (use_x) {
            spatial += clavis_led_map[i].x;
        }
        if (use_y) {
            spatial += clavis_led_map[i].y;
        }
        if (use_x && use_y) {
            spatial /= 2U;
        }

        uint8_t pulse = smooth_wave8((uint8_t)(phase + spatial));
        uint8_t brightness = scale_brightness(snapshot->brightness, pulse);
        set_pixel_hsv(i, snapshot->hue, snapshot->saturation, brightness);
    }
}

static void render_rainbow_spatial(const struct clavis_rgb_state *snapshot,
                                   uint32_t phase) {
    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (!led_selected(snapshot, i)) {
            continue;
        }

        uint16_t spatial_hue = (uint16_t)(((uint16_t)clavis_led_map[i].x * 180U) / 255U);
        spatial_hue += (uint16_t)(((uint16_t)clavis_led_map[i].y * 90U) / 255U);

        uint16_t hue = (snapshot->hue + spatial_hue + (phase % 360U)) % 360U;
        set_pixel_hsv(i, hue, snapshot->saturation, snapshot->brightness);
    }
}

static void render_chase(const struct clavis_rgb_state *snapshot, uint32_t phase) {
    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (!led_selected(snapshot, i)) {
            continue;
        }

        uint8_t local_phase = (uint8_t)(phase - ((uint16_t)i * 26U));
        uint8_t pulse = smooth_wave8(local_phase);

        /* Sharpen the pulse to look like a moving point with a tail. */
        pulse = (uint8_t)(((uint16_t)pulse * pulse) / 255U);
        uint8_t brightness = scale_brightness(snapshot->brightness, pulse);

        set_pixel_hsv(i, snapshot->hue, snapshot->saturation, brightness);
    }
}

static void render_index_scan(const struct clavis_rgb_state *snapshot, uint32_t phase) {
    uint8_t hold_frames = (uint8_t)MAX(6U, 24U - MIN(snapshot->speed, 10U));
    uint8_t index = (uint8_t)((phase / hold_frames) % CLAVIS_RGB_LED_COUNT);

    if (snapshot->reverse) {
        index = (CLAVIS_RGB_LED_COUNT - 1U) - index;
    }

    if (led_selected(snapshot, index)) {
        set_pixel_hsv(index, snapshot->hue, snapshot->saturation, snapshot->brightness);
    }

    /* A dim white marker on the manually selected LED. */
    if (snapshot->selected_led < CLAVIS_RGB_LED_COUNT &&
        led_selected(snapshot, snapshot->selected_led) &&
        snapshot->selected_led != index) {
        set_pixel_hsv(snapshot->selected_led, 0U, 0U,
                      MAX((uint8_t)5U, (uint8_t)(snapshot->brightness / 5U)));
    }
}

static bool effect_is_animated(uint8_t effect) {
    return effect != CLAVIS_RGB_EFFECT_STATIC;
}

static void render_frame(const struct clavis_rgb_state *snapshot, uint32_t phase) {
    clear_pixels();

    switch (snapshot->effect) {
    case CLAVIS_RGB_EFFECT_STATIC:
        render_static(snapshot);
        break;
    case CLAVIS_RGB_EFFECT_BREATHE:
        render_breathe(snapshot, phase);
        break;
    case CLAVIS_RGB_EFFECT_WAVE_X:
        render_wave_axis(snapshot, phase, true, false);
        break;
    case CLAVIS_RGB_EFFECT_WAVE_Y:
        render_wave_axis(snapshot, phase, false, true);
        break;
    case CLAVIS_RGB_EFFECT_WAVE_DIAGONAL:
        render_wave_axis(snapshot, phase, true, true);
        break;
    case CLAVIS_RGB_EFFECT_RAINBOW_SPATIAL:
        render_rainbow_spatial(snapshot, phase);
        break;
    case CLAVIS_RGB_EFFECT_CHASE:
        render_chase(snapshot, phase);
        break;
    case CLAVIS_RGB_EFFECT_INDEX_SCAN:
        render_index_scan(snapshot, phase);
        break;
    default:
        render_static(snapshot);
        break;
    }
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

    if (snapshot.on && effect_is_animated(snapshot.effect)) {
        uint32_t step = MAX(snapshot.speed, 1U);
        if (snapshot.reverse) {
            animation_phase = (animation_phase + CLAVIS_RGB_PHASE_MODULUS - step) %
                              CLAVIS_RGB_PHASE_MODULUS;
        } else {
            animation_phase = (animation_phase + step) % CLAVIS_RGB_PHASE_MODULUS;
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
            err = led_strip_update_rgb(strip, pixels, CLAVIS_RGB_LED_COUNT);

            if (err == 0) {
                k_mutex_lock(&state_lock, K_FOREVER);
                frame_dirty = false;
                strip_was_cleared = true;
                k_mutex_unlock(&state_lock);
            }
        }
    } else if (dirty || effect_is_animated(snapshot.effect)) {
        render_frame(&snapshot, phase);
        err = led_strip_update_rgb(strip, pixels, CLAVIS_RGB_LED_COUNT);

        if (err == 0) {
            k_mutex_lock(&state_lock, K_FOREVER);
            frame_dirty = false;
            strip_was_cleared = false;
            k_mutex_unlock(&state_lock);
        }

        next_delay_ms = effect_is_animated(snapshot.effect) ? CLAVIS_RGB_FRAME_MS
                                                            : CLAVIS_RGB_STATIC_POLL_MS;
    }

    if (err < 0) {
        LOG_ERR("led_strip_update_rgb failed: %d", err);
        next_delay_ms = 100;
    }

    k_work_schedule(&render_work, K_MSEC(next_delay_ms));
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

    int err = settings_save_one("clavis/rgb/state", &persisted, sizeof(persisted));
    if (err) {
        LOG_ERR("Unable to save RGB state: %d", err);
    }
}

static int settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;

    if (!settings_name_steq(name, "state", &next) || next != NULL) {
        return -ENOENT;
    }

    if (len != sizeof(struct clavis_rgb_persisted_state)) {
        return -EINVAL;
    }

    struct clavis_rgb_persisted_state persisted;
    int rc = read_cb(cb_arg, &persisted, sizeof(persisted));
    if (rc < 0) {
        return rc;
    }

    if (persisted.magic != CLAVIS_RGB_SETTINGS_MAGIC ||
        persisted.version != CLAVIS_RGB_SETTINGS_VERSION ||
        persisted.effect >= CLAVIS_RGB_EFFECT_COUNT || persisted.hue >= 360U ||
        persisted.saturation > 100U || persisted.brightness > 100U ||
        persisted.speed < 1U || persisted.speed > 10U ||
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

SETTINGS_STATIC_HANDLER_DEFINE(clavis_rgb_settings, "clavis/rgb", NULL, settings_set, NULL,
                               NULL);
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
    k_work_schedule(&render_work, K_MSEC(250));

    LOG_INF("Clavis RGB Matrix ready: %u LEDs", CLAVIS_RGB_LED_COUNT);
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
    int effect = (int)state.effect + (direction > 0 ? 1 : -1);
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

int clavis_rgb_set_hsb(uint16_t hue, uint8_t saturation, uint8_t brightness) {
    if (hue >= 360U || saturation > 100U || brightness > 100U) {
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

int clavis_rgb_change_hue(int direction) {
    k_mutex_lock(&state_lock, K_FOREVER);
    int hue = (int)state.hue + (direction * CLAVIS_RGB_HUE_STEP);
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
    int saturation = (int)state.saturation + (direction * CLAVIS_RGB_SAT_STEP);
    state.saturation = (uint8_t)CLAMP(saturation, 0, 100);
    state.on = true;
    k_mutex_unlock(&state_lock);

    mark_changed(false);
    return 0;
}

int clavis_rgb_change_brightness(int direction) {
    k_mutex_lock(&state_lock, K_FOREVER);
    int brightness = (int)state.brightness + (direction * CLAVIS_RGB_BRIGHTNESS_STEP);
    state.brightness = (uint8_t)CLAMP(brightness, 0, 100);
    state.on = true;
    k_mutex_unlock(&state_lock);

    mark_changed(false);
    return 0;
}

int clavis_rgb_change_speed(int direction) {
    k_mutex_lock(&state_lock, K_FOREVER);
    int speed = (int)state.speed + (direction * CLAVIS_RGB_SPEED_STEP);
    state.speed = (uint8_t)CLAMP(speed, 1, 10);
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
    int selected = (int)state.selected_led + (direction >= 0 ? 1 : -1);
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

const char *clavis_rgb_effect_name(uint8_t effect) {
    static const char *const names[CLAVIS_RGB_EFFECT_COUNT] = {
        [CLAVIS_RGB_EFFECT_STATIC] = "Static",
        [CLAVIS_RGB_EFFECT_BREATHE] = "Breathe",
        [CLAVIS_RGB_EFFECT_WAVE_X] = "Wave X",
        [CLAVIS_RGB_EFFECT_WAVE_Y] = "Wave Y",
        [CLAVIS_RGB_EFFECT_WAVE_DIAGONAL] = "Wave Diagonal",
        [CLAVIS_RGB_EFFECT_RAINBOW_SPATIAL] = "Rainbow Spatial",
        [CLAVIS_RGB_EFFECT_CHASE] = "Chase",
        [CLAVIS_RGB_EFFECT_INDEX_SCAN] = "Index Scan",
    };

    return effect < CLAVIS_RGB_EFFECT_COUNT ? names[effect] : "Unknown";
}
