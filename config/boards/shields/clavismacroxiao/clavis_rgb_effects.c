/*
 * ClavisMacro QMK-style RGB Matrix effects
 *
 * These effects reproduce the behavior categories of QMK RGB Matrix using
 * Clavis-native math, state tracking, and LED coordinates. They do not depend
 * on QMK internals.
 *
 * SPDX-License-Identifier: MIT
 */

#include "clavis_rgb_effects.h"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define CLAVIS_RGB_KEY_COUNT 9U
#define CLAVIS_RGB_HIT_HISTORY 8U

#define CLAVIS_CENTER_X 112
#define CLAVIS_CENTER_Y 112

struct clavis_rgb_hit {
    bool active;
    uint8_t key_id;
    uint8_t x;
    uint8_t y;
    uint16_t age;
};

struct clavis_rgb_runtime_snapshot {
    bool key_active[CLAVIS_RGB_KEY_COUNT];
    uint16_t key_age[CLAVIS_RGB_KEY_COUNT];
    uint8_t heat[CLAVIS_RGB_KEY_COUNT];

    struct clavis_rgb_hit hits[CLAVIS_RGB_HIT_HISTORY];

    uint8_t random_hue[CLAVIS_RGB_LED_COUNT];
    uint8_t random_sat[CLAVIS_RGB_LED_COUNT];

    uint8_t star_level[CLAVIS_RGB_LED_COUNT];
    uint8_t star_target[CLAVIS_RGB_LED_COUNT];
};

K_MUTEX_DEFINE(runtime_lock);

static bool runtime_initialized;
static uint32_t runtime_frame;
static uint32_t prng_state = 0x5247414EU; /* "RGAN"-ish deterministic seed */

static bool key_active[CLAVIS_RGB_KEY_COUNT];
static uint16_t key_age[CLAVIS_RGB_KEY_COUNT];
static uint8_t heat[CLAVIS_RGB_KEY_COUNT];

static struct clavis_rgb_hit hit_history[CLAVIS_RGB_HIT_HISTORY];
static uint8_t hit_head;

static uint8_t random_hue[CLAVIS_RGB_LED_COUNT];
static uint8_t random_sat[CLAVIS_RGB_LED_COUNT];
static uint8_t star_level[CLAVIS_RGB_LED_COUNT];
static uint8_t star_target[CLAVIS_RGB_LED_COUNT];

static uint32_t prng_next(void) {
    uint32_t x = prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    prng_state = x ? x : 0xA341316CU;
    return prng_state;
}

static uint8_t random8(void) {
    return (uint8_t)(prng_next() >> 24);
}

static uint8_t percent_to_u8(uint8_t percent) {
    return (uint8_t)(((uint16_t)MIN(percent, 100U) * 255U) / 100U);
}

static uint8_t u8_to_percent(uint8_t value) {
    return (uint8_t)(((uint16_t)value * 100U) / 255U);
}

static uint8_t scale_u8(uint8_t value, uint8_t scale) {
    return (uint8_t)(((uint16_t)value * scale) / 255U);
}

static uint8_t scale_percent(uint8_t percent, uint8_t scale) {
    return u8_to_percent(scale_u8(percent_to_u8(percent), scale));
}

static uint16_t hue_add8(uint16_t hue, uint8_t offset) {
    uint16_t add = (uint16_t)(((uint32_t)offset * 360U) / 256U);
    return (uint16_t)((hue + add) % 360U);
}

static uint16_t hue_add_signed(uint16_t hue, int16_t delta) {
    int32_t result = (int32_t)hue + delta;
    while (result < 0) {
        result += 360;
    }
    return (uint16_t)(result % 360);
}

static uint8_t triangle8(uint8_t phase) {
    return phase < 128U ? (uint8_t)(phase * 2U)
                        : (uint8_t)((255U - phase) * 2U);
}

static uint8_t smooth_wave8(uint8_t phase) {
    uint32_t x = triangle8(phase);
    uint32_t smooth = x * x * (765U - (2U * x));
    smooth /= (255U * 255U);
    return (uint8_t)MIN(smooth, 255U);
}

static int16_t iabs16(int16_t value) {
    return value < 0 ? -value : value;
}

static uint8_t distance8(int16_t dx, int16_t dy) {
    uint16_t ax = (uint16_t)iabs16(dx);
    uint16_t ay = (uint16_t)iabs16(dy);

    uint16_t maxv = MAX(ax, ay);
    uint16_t minv = MIN(ax, ay);

    return (uint8_t)MIN(255U, maxv + (minv >> 1));
}

static uint8_t angle8(int16_t dy, int16_t dx) {
    if (dx == 0 && dy == 0) {
        return 0;
    }

    uint16_t ax = (uint16_t)iabs16(dx);
    uint16_t ay = (uint16_t)iabs16(dy);
    uint8_t base;

    if (ax >= ay) {
        base = ax == 0U ? 0U : (uint8_t)((ay * 32U) / ax);
    } else {
        base = (uint8_t)(64U - ((ax * 32U) / ay));
    }

    if (dx >= 0 && dy >= 0) {
        return base;
    } else if (dx < 0 && dy >= 0) {
        return (uint8_t)(128U - base);
    } else if (dx < 0 && dy < 0) {
        return (uint8_t)(128U + base);
    } else {
        return (uint8_t)(0U - base);
    }
}

static uint8_t hash8(uint8_t a, uint8_t b, uint8_t c) {
    uint32_t x = 0x9E3779B9U;
    x ^= ((uint32_t)a + 0x7F4A7C15U) * 0x85EBCA6BU;
    x ^= ((uint32_t)b + 0x165667B1U) * 0xC2B2AE35U;
    x ^= ((uint32_t)c + 0x27D4EB2FU) * 0x27D4EB2DU;
    x ^= x >> 15;
    x *= 0x85EBCA6BU;
    x ^= x >> 13;
    return (uint8_t)(x >> 24);
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

static bool led_selected(const struct clavis_rgb_state *state, uint8_t index) {
    return (state->led_mask & BIT(index)) != 0U;
}

static void set_pixel_hsv(struct led_rgb pixels[CLAVIS_RGB_LED_COUNT],
                          uint8_t index,
                          uint16_t hue,
                          uint8_t saturation,
                          uint8_t brightness) {
    if (index >= CLAVIS_RGB_LED_COUNT) {
        return;
    }

    pixels[index] = hsv_to_rgb(hue, saturation, brightness);
}

static int led_for_key(uint8_t key_id) {
    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (clavis_led_map[i].key_id == key_id) {
            return i;
        }
    }
    return -1;
}

static void runtime_initialize_locked(void) {
    if (runtime_initialized) {
        return;
    }

    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        random_hue[i] = random8();
        random_sat[i] = (uint8_t)(128U + (random8() >> 1));
        star_target[i] = random8();
        star_level[i] = random8();
    }

    runtime_initialized = true;
}

static void runtime_tick(const struct clavis_rgb_state *state,
                         struct clavis_rgb_runtime_snapshot *snapshot) {
    const uint16_t age_step = (uint16_t)(1U + (state->speed / 16U));
    const uint8_t heat_decay = (uint8_t)(1U + (state->speed / 20U));
    const uint8_t random_interval = (uint8_t)MAX(2U, 18U - (state->speed / 6U));

    k_mutex_lock(&runtime_lock, K_FOREVER);

    runtime_initialize_locked();
    runtime_frame++;

    for (uint8_t key = 0; key < CLAVIS_RGB_KEY_COUNT; key++) {
        if (key_active[key]) {
            key_age[key] = (uint16_t)MIN(65535U, (uint32_t)key_age[key] + age_step);
        }

        heat[key] = heat[key] > heat_decay ? (uint8_t)(heat[key] - heat_decay) : 0U;
    }

    for (uint8_t i = 0; i < CLAVIS_RGB_HIT_HISTORY; i++) {
        if (!hit_history[i].active) {
            continue;
        }

        uint32_t next_age = (uint32_t)hit_history[i].age + age_step;
        if (next_age >= 900U) {
            hit_history[i].active = false;
        } else {
            hit_history[i].age = (uint16_t)next_age;
        }
    }

    if ((runtime_frame % random_interval) == 0U) {
        uint8_t led = (uint8_t)(random8() % CLAVIS_RGB_LED_COUNT);
        random_hue[led] = random8();
        random_sat[led] = (uint8_t)(96U + (random8() % 160U));
        star_target[led] = random8();
    }

    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (star_level[i] < star_target[i]) {
            star_level[i] = (uint8_t)MIN((uint16_t)star_level[i] + 4U,
                                         (uint16_t)star_target[i]);
        } else if (star_level[i] > star_target[i]) {
            star_level[i] = star_level[i] > 4U
                                ? (uint8_t)MAX((int)star_target[i],
                                               (int)star_level[i] - 4)
                                : star_target[i];
        }
    }

    memcpy(snapshot->key_active, key_active, sizeof(key_active));
    memcpy(snapshot->key_age, key_age, sizeof(key_age));
    memcpy(snapshot->heat, heat, sizeof(heat));
    memcpy(snapshot->hits, hit_history, sizeof(hit_history));
    memcpy(snapshot->random_hue, random_hue, sizeof(random_hue));
    memcpy(snapshot->random_sat, random_sat, sizeof(random_sat));
    memcpy(snapshot->star_level, star_level, sizeof(star_level));
    memcpy(snapshot->star_target, star_target, sizeof(star_target));

    k_mutex_unlock(&runtime_lock);
}

void clavis_rgb_effect_note_key_event(uint8_t key_id, bool pressed) {
    if (key_id >= CLAVIS_RGB_KEY_COUNT || !pressed) {
        return;
    }

    int led = led_for_key(key_id);
    if (led < 0) {
        return;
    }

    k_mutex_lock(&runtime_lock, K_FOREVER);
    runtime_initialize_locked();

    key_active[key_id] = true;
    key_age[key_id] = 0U;
    heat[key_id] = 255U;

    struct clavis_rgb_hit *hit = &hit_history[hit_head];
    hit->active = true;
    hit->key_id = key_id;
    hit->x = clavis_led_map[led].x;
    hit->y = clavis_led_map[led].y;
    hit->age = 0U;

    hit_head = (uint8_t)((hit_head + 1U) % CLAVIS_RGB_HIT_HISTORY);

    k_mutex_unlock(&runtime_lock);
}

static uint8_t fade_from_age(uint16_t age, uint8_t speed) {
    uint32_t scaled = ((uint32_t)age * (uint32_t)(3U + speed / 18U)) / 3U;
    return scaled >= 255U ? 0U : (uint8_t)(255U - scaled);
}

static uint8_t hit_radial_intensity(const struct clavis_rgb_hit *hit,
                                    uint8_t x,
                                    uint8_t y,
                                    uint8_t speed,
                                    bool ring) {
    if (!hit->active) {
        return 0U;
    }

    int16_t dx = (int16_t)x - hit->x;
    int16_t dy = (int16_t)y - hit->y;
    uint8_t dist = distance8(dx, dy);

    if (!ring) {
        uint32_t cost = (uint32_t)hit->age * (2U + speed / 35U) +
                        (uint32_t)dist;
        return cost >= 255U ? 0U : (uint8_t)(255U - cost);
    }

    uint16_t radius = (uint16_t)MIN(255U,
        ((uint32_t)hit->age * (3U + speed / 25U)) / 2U);

    uint16_t delta = radius > dist ? radius - dist : dist - radius;
    uint32_t cost = (uint32_t)delta * 6U + (hit->age / 3U);

    return cost >= 255U ? 0U : (uint8_t)(255U - cost);
}

static bool same_cross(uint8_t led, const struct clavis_rgb_hit *hit) {
    int16_t dx = (int16_t)clavis_led_map[led].x - hit->x;
    int16_t dy = (int16_t)clavis_led_map[led].y - hit->y;
    return iabs16(dx) < 36 || iabs16(dy) < 36;
}

static uint8_t hit_nexus_intensity(uint8_t led,
                                   const struct clavis_rgb_hit *hit,
                                   uint8_t speed) {
    if (!hit->active || !same_cross(led, hit)) {
        return 0U;
    }

    int16_t dx = (int16_t)clavis_led_map[led].x - hit->x;
    int16_t dy = (int16_t)clavis_led_map[led].y - hit->y;
    uint8_t axis_dist = (uint8_t)MAX(iabs16(dx), iabs16(dy));

    uint16_t travel = (uint16_t)MIN(255U,
        ((uint32_t)hit->age * (3U + speed / 20U)) / 2U);

    uint16_t delta = travel > axis_dist ? travel - axis_dist : axis_dist - travel;
    uint32_t cost = (uint32_t)delta * 7U + (hit->age / 3U);

    return cost >= 255U ? 0U : (uint8_t)(255U - cost);
}

static const struct clavis_rgb_hit *latest_hit(
    const struct clavis_rgb_runtime_snapshot *runtime) {
    const struct clavis_rgb_hit *best = NULL;

    for (uint8_t i = 0; i < CLAVIS_RGB_HIT_HISTORY; i++) {
        const struct clavis_rgb_hit *hit = &runtime->hits[i];

        if (!hit->active) {
            continue;
        }

        if (best == NULL || hit->age < best->age) {
            best = hit;
        }
    }

    return best;
}

static uint8_t max_multi_radial(const struct clavis_rgb_runtime_snapshot *runtime,
                                uint8_t x,
                                uint8_t y,
                                uint8_t speed,
                                bool ring) {
    uint8_t best = 0U;

    for (uint8_t i = 0; i < CLAVIS_RGB_HIT_HISTORY; i++) {
        best = MAX(best, hit_radial_intensity(&runtime->hits[i], x, y, speed, ring));
    }

    return best;
}

static uint8_t max_multi_cross(const struct clavis_rgb_runtime_snapshot *runtime,
                               uint8_t led,
                               uint8_t speed,
                               bool nexus) {
    uint8_t best = 0U;

    for (uint8_t i = 0; i < CLAVIS_RGB_HIT_HISTORY; i++) {
        const struct clavis_rgb_hit *hit = &runtime->hits[i];

        if (!hit->active) {
            continue;
        }

        uint8_t intensity;

        if (nexus) {
            intensity = hit_nexus_intensity(led, hit, speed);
        } else if (same_cross(led, hit)) {
            intensity = fade_from_age(hit->age, speed);
        } else {
            intensity = 0U;
        }

        best = MAX(best, intensity);
    }

    return best;
}

bool clavis_rgb_effect_is_animated(uint8_t effect) {
    switch (effect) {
    case CLAVIS_RGB_EFFECT_SOLID_COLOR:
    case CLAVIS_RGB_EFFECT_ALPHAS_MODS:
    case CLAVIS_RGB_EFFECT_GRADIENT_UP_DOWN:
    case CLAVIS_RGB_EFFECT_GRADIENT_LEFT_RIGHT:
        return false;
    default:
        return true;
    }
}

static void render_solid_color(const struct clavis_rgb_state *state,
                               struct led_rgb pixels[CLAVIS_RGB_LED_COUNT]) {
    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (led_selected(state, i)) {
            set_pixel_hsv(pixels, i, state->hue, state->saturation, state->brightness);
        }
    }
}

static void render_alphas_mods(const struct clavis_rgb_state *state,
                               struct led_rgb pixels[CLAVIS_RGB_LED_COUNT]) {
    /*
     * Clavis has macro keys rather than fixed alpha/modifier classifications.
     * The 9 Hall key LEDs use the primary hue and the encoder/aux LED uses the
     * secondary hue. This preserves the intended two-zone behavior.
     */
    uint16_t secondary = hue_add_signed(state->hue, 120);

    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (!led_selected(state, i)) {
            continue;
        }

        uint16_t hue = clavis_led_map[i].zone == CLAVIS_LED_ZONE_ENCODER
                           ? secondary
                           : state->hue;

        set_pixel_hsv(pixels, i, hue, state->saturation, state->brightness);
    }
}

static void render_gradient(const struct clavis_rgb_state *state,
                            struct led_rgb pixels[CLAVIS_RGB_LED_COUNT],
                            bool horizontal) {
    uint16_t span = (uint16_t)(20U + ((uint16_t)state->speed * 2U));

    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (!led_selected(state, i)) {
            continue;
        }

        uint8_t coord = horizontal ? clavis_led_map[i].x : clavis_led_map[i].y;
        uint16_t offset = (uint16_t)(((uint32_t)coord * span) / 255U);
        uint16_t hue = (uint16_t)((state->hue + offset) % 360U);

        set_pixel_hsv(pixels, i, hue, state->saturation, state->brightness);
    }
}

static void render_breathing(const struct clavis_rgb_state *state,
                             uint8_t time,
                             struct led_rgb pixels[CLAVIS_RGB_LED_COUNT]) {
    uint8_t intensity = smooth_wave8(time);
    uint8_t brightness = scale_percent(state->brightness, intensity);

    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (led_selected(state, i)) {
            set_pixel_hsv(pixels, i, state->hue, state->saturation, brightness);
        }
    }
}

static void render_band(const struct clavis_rgb_state *state,
                        uint8_t time,
                        struct led_rgb pixels[CLAVIS_RGB_LED_COUNT],
                        bool modify_sat,
                        uint8_t geometry) {
    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (!led_selected(state, i)) {
            continue;
        }

        int16_t dx = (int16_t)clavis_led_map[i].x - CLAVIS_CENTER_X;
        int16_t dy = (int16_t)clavis_led_map[i].y - CLAVIS_CENTER_Y;

        uint8_t spatial;

        switch (geometry) {
        case 1:
            spatial = (uint8_t)(angle8(dy, dx) * 3U);
            break;
        case 2:
            spatial = (uint8_t)(angle8(dy, dx) + distance8(dx, dy));
            break;
        default:
            spatial = (uint8_t)(clavis_led_map[i].x * 2U);
            break;
        }

        uint8_t wave = smooth_wave8((uint8_t)(time + spatial));
        uint8_t sat = state->saturation;
        uint8_t val = state->brightness;

        if (modify_sat) {
            sat = scale_percent(state->saturation, wave);
        } else {
            val = scale_percent(state->brightness, wave);
        }

        set_pixel_hsv(pixels, i, state->hue, sat, val);
    }
}

static void render_cycle(const struct clavis_rgb_state *state,
                         uint8_t time,
                         struct led_rgb pixels[CLAVIS_RGB_LED_COUNT],
                         uint8_t geometry) {
    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (!led_selected(state, i)) {
            continue;
        }

        int16_t dx = (int16_t)clavis_led_map[i].x - CLAVIS_CENTER_X;
        int16_t dy = (int16_t)clavis_led_map[i].y - CLAVIS_CENTER_Y;
        uint8_t offset = time;

        switch (geometry) {
        case 0: /* all */
            break;
        case 1: /* left/right */
            offset = (uint8_t)(offset + clavis_led_map[i].x);
            break;
        case 2: /* up/down */
            offset = (uint8_t)(offset + clavis_led_map[i].y);
            break;
        case 3: /* out/in */
            offset = (uint8_t)(offset + (uint8_t)(iabs16(dx) * 2));
            break;
        case 4: /* out/in dual */
            offset = (uint8_t)(offset + (uint8_t)(iabs16(dx) * 2) +
                               (dx < 0 ? 128U : 0U));
            break;
        case 5: /* moving chevron */
            offset = (uint8_t)(offset + clavis_led_map[i].x +
                               (uint8_t)iabs16(dy));
            break;
        case 6: /* pinwheel */
            offset = (uint8_t)(offset + angle8(dy, dx));
            break;
        case 7: /* spiral */
            offset = (uint8_t)(offset + angle8(dy, dx) + distance8(dx, dy));
            break;
        case 8: /* dual beacon */
            offset = (uint8_t)(offset + (uint8_t)(angle8(dy, dx) * 2U));
            break;
        case 9: /* rainbow beacon */
            offset = (uint8_t)(offset + (uint8_t)(angle8(dy, dx) * 4U));
            break;
        case 10: /* dual pinwheels */
            offset = (uint8_t)(offset + angle8(dy, dx) + (dx < 0 ? 128U : 0U));
            break;
        case 11: /* flower blooming */
            offset = (uint8_t)(offset +
                               (dy < 0 ? clavis_led_map[i].x
                                       : (uint8_t)(255U - clavis_led_map[i].x)));
            break;
        default:
            break;
        }

        set_pixel_hsv(pixels, i, hue_add8(state->hue, offset),
                      state->saturation, state->brightness);
    }
}

static void render_raindrops(const struct clavis_rgb_state *state,
                             const struct clavis_rgb_runtime_snapshot *runtime,
                             struct led_rgb pixels[CLAVIS_RGB_LED_COUNT],
                             bool jellybean) {
    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (!led_selected(state, i)) {
            continue;
        }

        uint16_t hue = hue_add8(state->hue, runtime->random_hue[i]);
        uint8_t sat = jellybean
                          ? MAX((uint8_t)25U, u8_to_percent(runtime->random_sat[i]))
                          : state->saturation;

        set_pixel_hsv(pixels, i, hue, sat, state->brightness);
    }
}

static void render_hue_motion(const struct clavis_rgb_state *state,
                              uint8_t time,
                              struct led_rgb pixels[CLAVIS_RGB_LED_COUNT],
                              uint8_t mode) {
    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (!led_selected(state, i)) {
            continue;
        }

        uint8_t phase = time;
        if (mode == 1U) {
            phase = (uint8_t)(time + clavis_led_map[i].x);
        } else if (mode == 2U) {
            phase = (uint8_t)(time + clavis_led_map[i].x * 2U);
        }

        int16_t delta = (int16_t)(smooth_wave8(phase) / 4U) - 32;
        uint16_t hue = hue_add_signed(state->hue, delta);

        set_pixel_hsv(pixels, i, hue, state->saturation, state->brightness);
    }
}

static void render_pixel_fractal(const struct clavis_rgb_state *state,
                                 uint8_t time,
                                 struct led_rgb pixels[CLAVIS_RGB_LED_COUNT]) {
    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (!led_selected(state, i)) {
            continue;
        }

        int16_t dx = (int16_t)clavis_led_map[i].x - CLAVIS_CENTER_X;
        uint8_t spatial = (uint8_t)(iabs16(dx) * 2U);
        uint8_t intensity = smooth_wave8((uint8_t)(time + spatial));
        uint8_t val = scale_percent(state->brightness, intensity);

        set_pixel_hsv(pixels, i, state->hue, state->saturation, val);
    }
}

static void render_pixel_flow(const struct clavis_rgb_state *state,
                              uint8_t time,
                              struct led_rgb pixels[CLAVIS_RGB_LED_COUNT]) {
    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (!led_selected(state, i)) {
            continue;
        }

        uint8_t local = (uint8_t)(time - (i * 28U));
        uint8_t intensity = smooth_wave8(local);
        intensity = scale_u8(intensity, intensity);

        uint8_t hue_offset = (uint8_t)(time + i * 41U);
        uint8_t val = scale_percent(state->brightness, intensity);

        set_pixel_hsv(pixels, i, hue_add8(state->hue, hue_offset),
                      state->saturation, val);
    }
}

static void render_pixel_rain(const struct clavis_rgb_state *state,
                              uint8_t time,
                              struct led_rgb pixels[CLAVIS_RGB_LED_COUNT]) {
    uint8_t epoch = (uint8_t)(time >> 3);

    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (!led_selected(state, i)) {
            continue;
        }

        uint8_t rnd = hash8(i, epoch, 0x52U);

        if (rnd < 82U) {
            set_pixel_hsv(pixels, i, hue_add8(state->hue, rnd),
                          state->saturation,
                          scale_percent(state->brightness,
                                        (uint8_t)(120U + (rnd >> 1))));
        }
    }
}

static void render_typing_heatmap(const struct clavis_rgb_state *state,
                                  const struct clavis_rgb_runtime_snapshot *runtime,
                                  struct led_rgb pixels[CLAVIS_RGB_LED_COUNT]) {
    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (!led_selected(state, i)) {
            continue;
        }

        uint8_t key = clavis_led_map[i].key_id;

        if (key == CLAVIS_LED_NO_KEY || key >= CLAVIS_RGB_KEY_COUNT) {
            set_pixel_hsv(pixels, i, 220U, 70U,
                          MAX((uint8_t)3U, (uint8_t)(state->brightness / 8U)));
            continue;
        }

        uint8_t h = runtime->heat[key];

        /* Cold blue -> cyan -> yellow -> hot red. */
        uint16_t hue = (uint16_t)(((uint16_t)(255U - h) * 240U) / 255U);
        uint8_t val = scale_percent(state->brightness,
                                    (uint8_t)MAX(38U, h));

        set_pixel_hsv(pixels, i, hue, 100U, val);
    }
}

static void render_digital_rain(const struct clavis_rgb_state *state,
                                uint8_t time,
                                struct led_rgb pixels[CLAVIS_RGB_LED_COUNT]) {
    static const uint8_t seed[3] = {0U, 2U, 4U};
    uint8_t step = (uint8_t)(time / 22U);

    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (!led_selected(state, i)) {
            continue;
        }

        if (clavis_led_map[i].zone == CLAVIS_LED_ZONE_ENCODER) {
            set_pixel_hsv(pixels, i, 120U, 100U,
                          MAX((uint8_t)2U, (uint8_t)(state->brightness / 10U)));
            continue;
        }

        uint8_t col = clavis_led_map[i].x < 72U ? 0U :
                      clavis_led_map[i].x < 152U ? 1U : 2U;

        uint8_t row = clavis_led_map[i].y < 72U ? 0U :
                      clavis_led_map[i].y < 152U ? 1U : 2U;

        uint8_t head = (uint8_t)((step + seed[col]) % 6U);
        uint8_t intensity = 0U;

        if (head == row) {
            intensity = 255U;
        } else if (((head + 5U) % 6U) == row) {
            intensity = 110U;
        } else if (((head + 4U) % 6U) == row) {
            intensity = 45U;
        }

        if (intensity > 0U) {
            set_pixel_hsv(pixels, i, 120U, 100U,
                          scale_percent(state->brightness, intensity));
        }
    }
}

static void render_reactive_simple(const struct clavis_rgb_state *state,
                                   const struct clavis_rgb_runtime_snapshot *runtime,
                                   struct led_rgb pixels[CLAVIS_RGB_LED_COUNT],
                                   bool hue_shift) {
    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (!led_selected(state, i)) {
            continue;
        }

        uint8_t key = clavis_led_map[i].key_id;

        if (key == CLAVIS_LED_NO_KEY || key >= CLAVIS_RGB_KEY_COUNT ||
            !runtime->key_active[key]) {
            if (hue_shift) {
                set_pixel_hsv(pixels, i, state->hue,
                              state->saturation, state->brightness);
            }
            continue;
        }

        uint8_t intensity = fade_from_age(runtime->key_age[key], state->speed);

        if (hue_shift) {
            uint16_t hue = hue_add8(state->hue, (uint8_t)(intensity / 2U));
            set_pixel_hsv(pixels, i, hue, state->saturation, state->brightness);
        } else {
            set_pixel_hsv(pixels, i, state->hue, state->saturation,
                          scale_percent(state->brightness, intensity));
        }
    }
}

static void render_reactive_radial(const struct clavis_rgb_state *state,
                                   const struct clavis_rgb_runtime_snapshot *runtime,
                                   struct led_rgb pixels[CLAVIS_RGB_LED_COUNT],
                                   bool multi,
                                   bool rainbow,
                                   bool ring) {
    const struct clavis_rgb_hit *latest = latest_hit(runtime);

    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (!led_selected(state, i)) {
            continue;
        }

        uint8_t intensity;

        if (multi) {
            intensity = max_multi_radial(runtime,
                                         clavis_led_map[i].x,
                                         clavis_led_map[i].y,
                                         state->speed,
                                         ring);
        } else if (latest != NULL) {
            intensity = hit_radial_intensity(latest,
                                             clavis_led_map[i].x,
                                             clavis_led_map[i].y,
                                             state->speed,
                                             ring);
        } else {
            intensity = 0U;
        }

        if (intensity == 0U) {
            continue;
        }

        uint16_t hue = state->hue;

        if (rainbow && latest != NULL) {
            int16_t dx = (int16_t)clavis_led_map[i].x - latest->x;
            int16_t dy = (int16_t)clavis_led_map[i].y - latest->y;
            hue = hue_add8(state->hue, distance8(dx, dy));
        }

        set_pixel_hsv(pixels, i, hue, state->saturation,
                      scale_percent(state->brightness, intensity));
    }
}

static void render_reactive_cross(const struct clavis_rgb_state *state,
                                  const struct clavis_rgb_runtime_snapshot *runtime,
                                  struct led_rgb pixels[CLAVIS_RGB_LED_COUNT],
                                  bool multi,
                                  bool nexus) {
    const struct clavis_rgb_hit *latest = latest_hit(runtime);

    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (!led_selected(state, i)) {
            continue;
        }

        uint8_t intensity = 0U;

        if (multi) {
            intensity = max_multi_cross(runtime, i, state->speed, nexus);
        } else if (latest != NULL) {
            if (nexus) {
                intensity = hit_nexus_intensity(i, latest, state->speed);
            } else if (same_cross(i, latest)) {
                intensity = fade_from_age(latest->age, state->speed);
            }
        }

        if (intensity > 0U) {
            set_pixel_hsv(pixels, i, state->hue, state->saturation,
                          scale_percent(state->brightness, intensity));
        }
    }
}

static void render_starlight(const struct clavis_rgb_state *state,
                             const struct clavis_rgb_runtime_snapshot *runtime,
                             uint8_t time,
                             struct led_rgb pixels[CLAVIS_RGB_LED_COUNT],
                             uint8_t mode) {
    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (!led_selected(state, i)) {
            continue;
        }

        uint8_t level;

        if (mode == 1U) {
            level = runtime->star_level[i];
        } else {
            uint8_t epoch = (uint8_t)(time >> 3);
            uint8_t rnd = hash8(i, epoch, 0xA7U);
            level = rnd > 150U ? rnd : 0U;
        }

        if (level == 0U) {
            continue;
        }

        uint16_t hue = state->hue;
        uint8_t sat = state->saturation;

        if (mode == 2U) {
            int16_t delta = (int16_t)((int8_t)(runtime->random_hue[i] & 0x3FU)) - 30;
            hue = hue_add_signed(hue, delta);
        } else if (mode == 3U) {
            int16_t delta = (int16_t)((int8_t)(runtime->random_sat[i] & 0x3FU)) - 30;
            sat = (uint8_t)CLAMP((int)state->saturation + delta, 0, 100);
        }

        set_pixel_hsv(pixels, i, hue, sat,
                      scale_percent(state->brightness, level));
    }
}

static void render_riverflow(const struct clavis_rgb_state *state,
                             uint8_t time,
                             struct led_rgb pixels[CLAVIS_RGB_LED_COUNT]) {
    for (uint8_t i = 0; i < CLAVIS_RGB_LED_COUNT; i++) {
        if (!led_selected(state, i)) {
            continue;
        }

        uint8_t offset = (uint8_t)(
            ((uint16_t)clavis_led_map[i].x * 2U) +
            ((uint16_t)clavis_led_map[i].y / 2U)
        );

        uint8_t intensity = smooth_wave8((uint8_t)(time + offset));
        uint8_t val = scale_percent(state->brightness, intensity);

        set_pixel_hsv(pixels, i, state->hue, state->saturation, val);
    }
}

void clavis_rgb_effect_render(const struct clavis_rgb_state *state,
                              uint32_t phase,
                              struct led_rgb pixels[CLAVIS_RGB_LED_COUNT]) {
    struct clavis_rgb_runtime_snapshot runtime;
    runtime_tick(state, &runtime);

    memset(pixels, 0, sizeof(struct led_rgb) * CLAVIS_RGB_LED_COUNT);

    uint8_t time = (uint8_t)phase;

    switch (state->effect) {
    case CLAVIS_RGB_EFFECT_SOLID_COLOR:
        render_solid_color(state, pixels);
        break;

    case CLAVIS_RGB_EFFECT_ALPHAS_MODS:
        render_alphas_mods(state, pixels);
        break;

    case CLAVIS_RGB_EFFECT_GRADIENT_UP_DOWN:
        render_gradient(state, pixels, false);
        break;

    case CLAVIS_RGB_EFFECT_GRADIENT_LEFT_RIGHT:
        render_gradient(state, pixels, true);
        break;

    case CLAVIS_RGB_EFFECT_BREATHING:
        render_breathing(state, time, pixels);
        break;

    case CLAVIS_RGB_EFFECT_BAND_SAT:
        render_band(state, time, pixels, true, 0);
        break;

    case CLAVIS_RGB_EFFECT_BAND_VAL:
        render_band(state, time, pixels, false, 0);
        break;

    case CLAVIS_RGB_EFFECT_BAND_PINWHEEL_SAT:
        render_band(state, time, pixels, true, 1);
        break;

    case CLAVIS_RGB_EFFECT_BAND_PINWHEEL_VAL:
        render_band(state, time, pixels, false, 1);
        break;

    case CLAVIS_RGB_EFFECT_BAND_SPIRAL_SAT:
        render_band(state, time, pixels, true, 2);
        break;

    case CLAVIS_RGB_EFFECT_BAND_SPIRAL_VAL:
        render_band(state, time, pixels, false, 2);
        break;

    case CLAVIS_RGB_EFFECT_CYCLE_ALL:
        render_cycle(state, time, pixels, 0);
        break;

    case CLAVIS_RGB_EFFECT_CYCLE_LEFT_RIGHT:
        render_cycle(state, time, pixels, 1);
        break;

    case CLAVIS_RGB_EFFECT_CYCLE_UP_DOWN:
        render_cycle(state, time, pixels, 2);
        break;

    case CLAVIS_RGB_EFFECT_CYCLE_OUT_IN:
        render_cycle(state, time, pixels, 3);
        break;

    case CLAVIS_RGB_EFFECT_CYCLE_OUT_IN_DUAL:
        render_cycle(state, time, pixels, 4);
        break;

    case CLAVIS_RGB_EFFECT_RAINBOW_MOVING_CHEVRON:
        render_cycle(state, time, pixels, 5);
        break;

    case CLAVIS_RGB_EFFECT_CYCLE_PINWHEEL:
        render_cycle(state, time, pixels, 6);
        break;

    case CLAVIS_RGB_EFFECT_CYCLE_SPIRAL:
        render_cycle(state, time, pixels, 7);
        break;

    case CLAVIS_RGB_EFFECT_DUAL_BEACON:
        render_cycle(state, time, pixels, 8);
        break;

    case CLAVIS_RGB_EFFECT_RAINBOW_BEACON:
        render_cycle(state, time, pixels, 9);
        break;

    case CLAVIS_RGB_EFFECT_RAINBOW_PINWHEELS:
        render_cycle(state, time, pixels, 10);
        break;

    case CLAVIS_RGB_EFFECT_FLOWER_BLOOMING:
        render_cycle(state, time, pixels, 11);
        break;

    case CLAVIS_RGB_EFFECT_RAINDROPS:
        render_raindrops(state, &runtime, pixels, false);
        break;

    case CLAVIS_RGB_EFFECT_JELLYBEAN_RAINDROPS:
        render_raindrops(state, &runtime, pixels, true);
        break;

    case CLAVIS_RGB_EFFECT_HUE_BREATHING:
        render_hue_motion(state, time, pixels, 0);
        break;

    case CLAVIS_RGB_EFFECT_HUE_PENDULUM:
        render_hue_motion(state, time, pixels, 1);
        break;

    case CLAVIS_RGB_EFFECT_HUE_WAVE:
        render_hue_motion(state, time, pixels, 2);
        break;

    case CLAVIS_RGB_EFFECT_PIXEL_FRACTAL:
        render_pixel_fractal(state, time, pixels);
        break;

    case CLAVIS_RGB_EFFECT_PIXEL_FLOW:
        render_pixel_flow(state, time, pixels);
        break;

    case CLAVIS_RGB_EFFECT_PIXEL_RAIN:
        render_pixel_rain(state, time, pixels);
        break;

    case CLAVIS_RGB_EFFECT_TYPING_HEATMAP:
        render_typing_heatmap(state, &runtime, pixels);
        break;

    case CLAVIS_RGB_EFFECT_DIGITAL_RAIN:
        render_digital_rain(state, time, pixels);
        break;

    case CLAVIS_RGB_EFFECT_SOLID_REACTIVE_SIMPLE:
        render_reactive_simple(state, &runtime, pixels, false);
        break;

    case CLAVIS_RGB_EFFECT_SOLID_REACTIVE:
        render_reactive_simple(state, &runtime, pixels, true);
        break;

    case CLAVIS_RGB_EFFECT_SOLID_REACTIVE_WIDE:
        render_reactive_radial(state, &runtime, pixels, false, false, false);
        break;

    case CLAVIS_RGB_EFFECT_SOLID_REACTIVE_MULTIWIDE:
        render_reactive_radial(state, &runtime, pixels, true, false, false);
        break;

    case CLAVIS_RGB_EFFECT_SOLID_REACTIVE_CROSS:
        render_reactive_cross(state, &runtime, pixels, false, false);
        break;

    case CLAVIS_RGB_EFFECT_SOLID_REACTIVE_MULTICROSS:
        render_reactive_cross(state, &runtime, pixels, true, false);
        break;

    case CLAVIS_RGB_EFFECT_SOLID_REACTIVE_NEXUS:
        render_reactive_cross(state, &runtime, pixels, false, true);
        break;

    case CLAVIS_RGB_EFFECT_SOLID_REACTIVE_MULTINEXUS:
        render_reactive_cross(state, &runtime, pixels, true, true);
        break;

    case CLAVIS_RGB_EFFECT_SPLASH:
        render_reactive_radial(state, &runtime, pixels, false, true, true);
        break;

    case CLAVIS_RGB_EFFECT_MULTISPLASH:
        render_reactive_radial(state, &runtime, pixels, true, true, true);
        break;

    case CLAVIS_RGB_EFFECT_SOLID_SPLASH:
        render_reactive_radial(state, &runtime, pixels, false, false, true);
        break;

    case CLAVIS_RGB_EFFECT_SOLID_MULTISPLASH:
        render_reactive_radial(state, &runtime, pixels, true, false, true);
        break;

    case CLAVIS_RGB_EFFECT_STARLIGHT:
        render_starlight(state, &runtime, time, pixels, 0);
        break;

    case CLAVIS_RGB_EFFECT_STARLIGHT_SMOOTH:
        render_starlight(state, &runtime, time, pixels, 1);
        break;

    case CLAVIS_RGB_EFFECT_STARLIGHT_DUAL_HUE:
        render_starlight(state, &runtime, time, pixels, 2);
        break;

    case CLAVIS_RGB_EFFECT_STARLIGHT_DUAL_SAT:
        render_starlight(state, &runtime, time, pixels, 3);
        break;

    case CLAVIS_RGB_EFFECT_RIVERFLOW:
        render_riverflow(state, time, pixels);
        break;

    default:
        render_solid_color(state, pixels);
        break;
    }
}

const char *clavis_rgb_effect_name(uint8_t effect) {
    static const char *const names[CLAVIS_RGB_EFFECT_COUNT] = {
        [CLAVIS_RGB_EFFECT_SOLID_COLOR] = "Solid Color",
        [CLAVIS_RGB_EFFECT_ALPHAS_MODS] = "Alphas Mods",
        [CLAVIS_RGB_EFFECT_GRADIENT_UP_DOWN] = "Gradient Up/Down",
        [CLAVIS_RGB_EFFECT_GRADIENT_LEFT_RIGHT] = "Gradient Left/Right",
        [CLAVIS_RGB_EFFECT_BREATHING] = "Breathing",
        [CLAVIS_RGB_EFFECT_BAND_SAT] = "Band Saturation",
        [CLAVIS_RGB_EFFECT_BAND_VAL] = "Band Value",
        [CLAVIS_RGB_EFFECT_BAND_PINWHEEL_SAT] = "Band Pinwheel Saturation",
        [CLAVIS_RGB_EFFECT_BAND_PINWHEEL_VAL] = "Band Pinwheel Value",
        [CLAVIS_RGB_EFFECT_BAND_SPIRAL_SAT] = "Band Spiral Saturation",
        [CLAVIS_RGB_EFFECT_BAND_SPIRAL_VAL] = "Band Spiral Value",
        [CLAVIS_RGB_EFFECT_CYCLE_ALL] = "Cycle All",
        [CLAVIS_RGB_EFFECT_CYCLE_LEFT_RIGHT] = "Cycle Left/Right",
        [CLAVIS_RGB_EFFECT_CYCLE_UP_DOWN] = "Cycle Up/Down",
        [CLAVIS_RGB_EFFECT_CYCLE_OUT_IN] = "Cycle Out/In",
        [CLAVIS_RGB_EFFECT_CYCLE_OUT_IN_DUAL] = "Cycle Out/In Dual",
        [CLAVIS_RGB_EFFECT_RAINBOW_MOVING_CHEVRON] = "Rainbow Moving Chevron",
        [CLAVIS_RGB_EFFECT_CYCLE_PINWHEEL] = "Cycle Pinwheel",
        [CLAVIS_RGB_EFFECT_CYCLE_SPIRAL] = "Cycle Spiral",
        [CLAVIS_RGB_EFFECT_DUAL_BEACON] = "Dual Beacon",
        [CLAVIS_RGB_EFFECT_RAINBOW_BEACON] = "Rainbow Beacon",
        [CLAVIS_RGB_EFFECT_RAINBOW_PINWHEELS] = "Rainbow Pinwheels",
        [CLAVIS_RGB_EFFECT_FLOWER_BLOOMING] = "Flower Blooming",
        [CLAVIS_RGB_EFFECT_RAINDROPS] = "Raindrops",
        [CLAVIS_RGB_EFFECT_JELLYBEAN_RAINDROPS] = "Jellybean Raindrops",
        [CLAVIS_RGB_EFFECT_HUE_BREATHING] = "Hue Breathing",
        [CLAVIS_RGB_EFFECT_HUE_PENDULUM] = "Hue Pendulum",
        [CLAVIS_RGB_EFFECT_HUE_WAVE] = "Hue Wave",
        [CLAVIS_RGB_EFFECT_PIXEL_FRACTAL] = "Pixel Fractal",
        [CLAVIS_RGB_EFFECT_PIXEL_FLOW] = "Pixel Flow",
        [CLAVIS_RGB_EFFECT_PIXEL_RAIN] = "Pixel Rain",
        [CLAVIS_RGB_EFFECT_TYPING_HEATMAP] = "Typing Heatmap",
        [CLAVIS_RGB_EFFECT_DIGITAL_RAIN] = "Digital Rain",
        [CLAVIS_RGB_EFFECT_SOLID_REACTIVE_SIMPLE] = "Solid Reactive Simple",
        [CLAVIS_RGB_EFFECT_SOLID_REACTIVE] = "Solid Reactive",
        [CLAVIS_RGB_EFFECT_SOLID_REACTIVE_WIDE] = "Solid Reactive Wide",
        [CLAVIS_RGB_EFFECT_SOLID_REACTIVE_MULTIWIDE] = "Solid Reactive Multiwide",
        [CLAVIS_RGB_EFFECT_SOLID_REACTIVE_CROSS] = "Solid Reactive Cross",
        [CLAVIS_RGB_EFFECT_SOLID_REACTIVE_MULTICROSS] = "Solid Reactive Multicross",
        [CLAVIS_RGB_EFFECT_SOLID_REACTIVE_NEXUS] = "Solid Reactive Nexus",
        [CLAVIS_RGB_EFFECT_SOLID_REACTIVE_MULTINEXUS] = "Solid Reactive Multinexus",
        [CLAVIS_RGB_EFFECT_SPLASH] = "Splash",
        [CLAVIS_RGB_EFFECT_MULTISPLASH] = "Multisplash",
        [CLAVIS_RGB_EFFECT_SOLID_SPLASH] = "Solid Splash",
        [CLAVIS_RGB_EFFECT_SOLID_MULTISPLASH] = "Solid Multisplash",
        [CLAVIS_RGB_EFFECT_STARLIGHT] = "Starlight",
        [CLAVIS_RGB_EFFECT_STARLIGHT_SMOOTH] = "Starlight Smooth",
        [CLAVIS_RGB_EFFECT_STARLIGHT_DUAL_HUE] = "Starlight Dual Hue",
        [CLAVIS_RGB_EFFECT_STARLIGHT_DUAL_SAT] = "Starlight Dual Saturation",
        [CLAVIS_RGB_EFFECT_RIVERFLOW] = "Riverflow",
    };

    return effect < CLAVIS_RGB_EFFECT_COUNT ? names[effect] : "Unknown";
}
