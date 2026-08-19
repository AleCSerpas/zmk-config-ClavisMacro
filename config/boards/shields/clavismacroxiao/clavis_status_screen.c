/*
 * ClavisMacro custom 240x240 status screen.
 *
 * Current data:
 *   - Active ZMK layer
 *   - RGB brightness with effect-aware color ring
 *   - Battery percentage; hidden when ZMK reports 0%
 *
 * RGB ring behavior:
 *   - Single-color effects: current selected hue
 *   - Rainbow effects: RGB spectrum around the ring
 *   - Two-color effects: hue -> secondary hue -> hue
 *   - Paint Mode: ring uses the actual 10 painted LED colors
 *
 * Placeholder:
 *   - Volume remains fixed at 36% until host volume feedback is available.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include <lvgl.h>

#include <zmk/battery.h>
#include <zmk/display/status_screen.h>
#include <zmk/keymap.h>

#include "clavis_rgb_engine.h"

#define UI_UPDATE_PERIOD_MS 250
#define VOLUME_PLACEHOLDER 36

#define COLOR_BACKGROUND 0x151617
#define COLOR_PANEL      0x1C1E20
#define COLOR_WHITE      0xF7F7F7
#define COLOR_TRACK      0x3A3A3A
#define COLOR_SHADOW     0x000000

#define RGB_RING_X 126
#define RGB_RING_Y 8
#define RGB_RING_SIZE 50
#define RGB_RING_WIDTH 7
#define RGB_RING_SEGMENTS 12

static lv_obj_t *layer_label;

static lv_obj_t *rgb_track;
static lv_obj_t *rgb_segments[RGB_RING_SEGMENTS];
static lv_obj_t *rgb_label;

static lv_obj_t *battery_group;
static lv_obj_t *battery_arc;
static lv_obj_t *battery_label;

static lv_obj_t *volume_bar;
static lv_obj_t *volume_label;

static lv_obj_t *create_ring(lv_obj_t *parent, int32_t x, int32_t y) {
    lv_obj_t *arc = lv_arc_create(parent);

    lv_obj_remove_style_all(arc);
    lv_obj_set_pos(arc, x, y);
    lv_obj_set_size(arc, RGB_RING_SIZE, RGB_RING_SIZE);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_SCROLLABLE);

    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);

    lv_obj_set_style_arc_width(arc, RGB_RING_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_set_style_arc_width(arc, RGB_RING_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COLOR_WHITE), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);

    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);

    return arc;
}

static lv_obj_t *create_rgb_track(lv_obj_t *parent) {
    lv_obj_t *arc = lv_arc_create(parent);

    lv_obj_remove_style_all(arc);
    lv_obj_set_pos(arc, RGB_RING_X, RGB_RING_Y);
    lv_obj_set_size(arc, RGB_RING_SIZE, RGB_RING_SIZE);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_SCROLLABLE);

    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);

    lv_obj_set_style_arc_width(arc, RGB_RING_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);

    return arc;
}

static lv_obj_t *create_rgb_segment(lv_obj_t *parent) {
    lv_obj_t *arc = lv_arc_create(parent);

    lv_obj_remove_style_all(arc);
    lv_obj_set_pos(arc, RGB_RING_X, RGB_RING_Y);
    lv_obj_set_size(arc, RGB_RING_SIZE, RGB_RING_SIZE);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_SCROLLABLE);

    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 1);

    lv_obj_set_style_arc_width(arc, RGB_RING_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);

    lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);

    return arc;
}

static lv_obj_t *create_ring_label(lv_obj_t *parent, int32_t x, int32_t y) {
    lv_obj_t *label = lv_label_create(parent);

    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, 50, 20);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_WHITE), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);

    return label;
}

static void set_percent_label(lv_obj_t *label, uint8_t value) {
    char text[8] = {};

    snprintf(text, sizeof(text), "%u%%", value);
    lv_label_set_text(label, text);

    /*
     * "100%" is the only normal value that gets tight inside a 50 px ring.
     * Keep the regular 16 px font for 0..99 and automatically shrink 100.
     */
    lv_obj_set_style_text_font(
        label,
        value >= 100U
            ? &lv_font_montserrat_14
            : &lv_font_montserrat_16,
        LV_PART_MAIN
    );
}

static lv_color_t rgb888_color(struct clavis_rgb_color color) {
    uint32_t hex =
        ((uint32_t)color.r << 16) |
        ((uint32_t)color.g << 8) |
        color.b;

    return lv_color_hex(hex);
}

static bool effect_is_rainbow(uint8_t effect) {
    switch (effect) {
    case CLAVIS_RGB_EFFECT_CYCLE_ALL:
    case CLAVIS_RGB_EFFECT_CYCLE_LEFT_RIGHT:
    case CLAVIS_RGB_EFFECT_CYCLE_UP_DOWN:
    case CLAVIS_RGB_EFFECT_CYCLE_OUT_IN:
    case CLAVIS_RGB_EFFECT_CYCLE_OUT_IN_DUAL:
    case CLAVIS_RGB_EFFECT_RAINBOW_MOVING_CHEVRON:
    case CLAVIS_RGB_EFFECT_CYCLE_PINWHEEL:
    case CLAVIS_RGB_EFFECT_CYCLE_SPIRAL:
    case CLAVIS_RGB_EFFECT_DUAL_BEACON:
    case CLAVIS_RGB_EFFECT_RAINBOW_BEACON:
    case CLAVIS_RGB_EFFECT_RAINBOW_PINWHEELS:
    case CLAVIS_RGB_EFFECT_FLOWER_BLOOMING:
    case CLAVIS_RGB_EFFECT_RAINDROPS:
    case CLAVIS_RGB_EFFECT_JELLYBEAN_RAINDROPS:
    case CLAVIS_RGB_EFFECT_PIXEL_FLOW:
    case CLAVIS_RGB_EFFECT_PIXEL_RAIN:
    case CLAVIS_RGB_EFFECT_TYPING_HEATMAP:
    case CLAVIS_RGB_EFFECT_DIGITAL_RAIN:
    case CLAVIS_RGB_EFFECT_SPLASH:
    case CLAVIS_RGB_EFFECT_MULTISPLASH:
    case CLAVIS_RGB_EFFECT_STARLIGHT_DUAL_HUE:
    case CLAVIS_RGB_EFFECT_STARLIGHT_DUAL_SAT:
        return true;

    default:
        return false;
    }
}

static bool effect_is_two_color(uint8_t effect) {
    switch (effect) {
    case CLAVIS_RGB_EFFECT_ALPHAS_MODS:
    case CLAVIS_RGB_EFFECT_GRADIENT_UP_DOWN:
    case CLAVIS_RGB_EFFECT_GRADIENT_LEFT_RIGHT:
    case CLAVIS_RGB_EFFECT_HUE_BREATHING:
    case CLAVIS_RGB_EFFECT_HUE_PENDULUM:
    case CLAVIS_RGB_EFFECT_HUE_WAVE:
        return true;

    default:
        return false;
    }
}

static lv_color_t rgb_ring_segment_color(
    const struct clavis_rgb_state *rgb_state,
    uint8_t segment
) {
    if (rgb_state->paint_mode) {
        uint8_t paint_index =
            (uint8_t)(
                ((uint16_t)segment * CLAVIS_RGB_LED_COUNT) /
                RGB_RING_SEGMENTS
            );

        paint_index =
            MIN(paint_index, CLAVIS_RGB_LED_COUNT - 1U);

        return rgb888_color(
            rgb_state->paint_colors[paint_index]
        );
    }

    if (effect_is_rainbow(rgb_state->effect)) {
        uint16_t hue =
            (uint16_t)(
                (
                    rgb_state->hue +
                    ((uint32_t)segment * 360U) /
                    RGB_RING_SEGMENTS
                ) %
                360U
            );

        return lv_color_hsv_to_rgb(
            hue,
            100,
            100
        );
    }

    if (effect_is_two_color(rgb_state->effect)) {
        const uint8_t half =
            RGB_RING_SEGMENTS / 2U;

        uint16_t offset;

        if (segment <= half) {
            offset =
                (uint16_t)(
                    ((uint32_t)segment * 120U) /
                    MAX(half, 1U)
                );
        } else {
            offset =
                (uint16_t)(
                    ((uint32_t)(RGB_RING_SEGMENTS - segment) * 120U) /
                    MAX(half, 1U)
                );
        }

        return lv_color_hsv_to_rgb(
            (rgb_state->hue + offset) % 360U,
            rgb_state->saturation,
            100
        );
    }

    return lv_color_hsv_to_rgb(
        rgb_state->hue % 360U,
        rgb_state->saturation,
        100
    );
}

static void update_rgb_segments(
    const struct clavis_rgb_state *rgb_state,
    uint8_t brightness
) {
    for (uint8_t i = 0; i < RGB_RING_SEGMENTS; i++) {
        const uint16_t start_percent =
            ((uint16_t)i * 100U) /
            RGB_RING_SEGMENTS;

        const uint16_t end_percent =
            ((uint16_t)(i + 1U) * 100U) /
            RGB_RING_SEGMENTS;

        if (!rgb_state->on ||
            brightness <= start_percent) {

            lv_obj_add_flag(
                rgb_segments[i],
                LV_OBJ_FLAG_HIDDEN
            );

            continue;
        }

        uint16_t visible_end =
            MIN((uint16_t)brightness, end_percent);

        uint16_t start_angle =
            (start_percent * 360U) / 100U;

        uint16_t end_angle =
            (visible_end * 360U) / 100U;

        if (end_angle <= start_angle) {
            lv_obj_add_flag(
                rgb_segments[i],
                LV_OBJ_FLAG_HIDDEN
            );

            continue;
        }

        lv_arc_set_bg_angles(
            rgb_segments[i],
            start_angle,
            end_angle
        );

        lv_obj_set_style_arc_color(
            rgb_segments[i],
            rgb_ring_segment_color(
                rgb_state,
                i
            ),
            LV_PART_MAIN
        );

        lv_obj_remove_flag(
            rgb_segments[i],
            LV_OBJ_FLAG_HIDDEN
        );
    }
}

static void update_layer(void) {
    zmk_keymap_layer_index_t index =
        zmk_keymap_highest_layer_active();

    const char *name =
        zmk_keymap_layer_name(
            zmk_keymap_layer_index_to_id(index)
        );

    char text[20] = {};

    if (name != NULL && strlen(name) > 0) {
        snprintf(text, sizeof(text), "%s", name);
    } else {
        snprintf(
            text,
            sizeof(text),
            "Layer %u",
            (unsigned int)index
        );
    }

    lv_label_set_text(layer_label, text);
}

static void update_rgb(void) {
    struct clavis_rgb_state rgb_state = {0};

    if (clavis_rgb_get_state(&rgb_state) < 0) {
        set_percent_label(rgb_label, 0);

        for (uint8_t i = 0; i < RGB_RING_SEGMENTS; i++) {
            lv_obj_add_flag(
                rgb_segments[i],
                LV_OBJ_FLAG_HIDDEN
            );
        }

        return;
    }

    uint8_t brightness =
        rgb_state.on
            ? CLAMP(rgb_state.brightness, 0, 100)
            : 0;

    set_percent_label(
        rgb_label,
        brightness
    );

    update_rgb_segments(
        &rgb_state,
        brightness
    );
}

static void update_battery(void) {
    uint8_t level =
        CLAMP(
            zmk_battery_state_of_charge(),
            0,
            100
        );

    if (level == 0) {
        lv_obj_add_flag(
            battery_group,
            LV_OBJ_FLAG_HIDDEN
        );

        return;
    }

    lv_obj_remove_flag(
        battery_group,
        LV_OBJ_FLAG_HIDDEN
    );

    lv_arc_set_value(
        battery_arc,
        level
    );

    set_percent_label(
        battery_label,
        level
    );
}

static void update_ui(lv_timer_t *timer) {
    ARG_UNUSED(timer);

    update_layer();
    update_rgb();
    update_battery();
}

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen =
        lv_obj_create(NULL);

    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, 240, 240);

    lv_obj_set_style_bg_color(
        screen,
        lv_color_hex(COLOR_BACKGROUND),
        LV_PART_MAIN
    );

    lv_obj_set_style_bg_opa(
        screen,
        LV_OPA_COVER,
        LV_PART_MAIN
    );

    lv_obj_remove_flag(
        screen,
        LV_OBJ_FLAG_SCROLLABLE
    );

    /* RGB brightness ring — segmented so it can show a real RGB spectrum. */
    rgb_track =
        create_rgb_track(screen);

    ARG_UNUSED(rgb_track);

    for (uint8_t i = 0; i < RGB_RING_SEGMENTS; i++) {
        rgb_segments[i] =
            create_rgb_segment(screen);
    }

    rgb_label =
        create_ring_label(
            screen,
            RGB_RING_X,
            RGB_RING_Y + 15
        );

    /* Battery ring — top-right. */
    battery_group =
        lv_obj_create(screen);

    lv_obj_remove_style_all(
        battery_group
    );

    lv_obj_set_pos(
        battery_group,
        182,
        8
    );

    lv_obj_set_size(
        battery_group,
        50,
        50
    );

    lv_obj_remove_flag(
        battery_group,
        LV_OBJ_FLAG_SCROLLABLE
    );

    battery_arc =
        create_ring(
            battery_group,
            0,
            0
        );

    battery_label =
        create_ring_label(
            battery_group,
            0,
            15
        );

    lv_obj_set_style_arc_color(
        battery_arc,
        lv_color_hex(COLOR_WHITE),
        LV_PART_INDICATOR
    );

    /* Central rounded layer panel. */
    lv_obj_t *layer_panel =
        lv_obj_create(screen);

    lv_obj_remove_style_all(
        layer_panel
    );

    lv_obj_set_pos(
        layer_panel,
        43,
        73
    );

    lv_obj_set_size(
        layer_panel,
        154,
        96
    );

    lv_obj_set_style_bg_color(
        layer_panel,
        lv_color_hex(COLOR_PANEL),
        LV_PART_MAIN
    );

    lv_obj_set_style_bg_opa(
        layer_panel,
        LV_OPA_COVER,
        LV_PART_MAIN
    );

    lv_obj_set_style_border_color(
        layer_panel,
        lv_color_hex(COLOR_WHITE),
        LV_PART_MAIN
    );

    lv_obj_set_style_border_width(
        layer_panel,
        3,
        LV_PART_MAIN
    );

    lv_obj_set_style_radius(
        layer_panel,
        24,
        LV_PART_MAIN
    );

    lv_obj_set_style_shadow_color(
        layer_panel,
        lv_color_hex(COLOR_SHADOW),
        LV_PART_MAIN
    );

    lv_obj_set_style_shadow_width(
        layer_panel,
        10,
        LV_PART_MAIN
    );

    lv_obj_set_style_shadow_opa(
        layer_panel,
        LV_OPA_50,
        LV_PART_MAIN
    );

    lv_obj_set_style_shadow_offset_x(
        layer_panel,
        4,
        LV_PART_MAIN
    );

    lv_obj_set_style_shadow_offset_y(
        layer_panel,
        5,
        LV_PART_MAIN
    );

    lv_obj_remove_flag(
        layer_panel,
        LV_OBJ_FLAG_SCROLLABLE
    );

    layer_label =
        lv_label_create(layer_panel);

    lv_obj_set_style_text_color(
        layer_label,
        lv_color_hex(COLOR_WHITE),
        LV_PART_MAIN
    );

    lv_obj_set_style_text_font(
        layer_label,
        &lv_font_montserrat_24,
        LV_PART_MAIN
    );

    lv_obj_set_style_text_align(
        layer_label,
        LV_TEXT_ALIGN_CENTER,
        LV_PART_MAIN
    );

    lv_obj_center(
        layer_label
    );

    /* Volume placeholder. */
    volume_bar =
        lv_bar_create(screen);

    lv_obj_remove_style_all(
        volume_bar
    );

    lv_obj_set_pos(
        volume_bar,
        28,
        211
    );

    lv_obj_set_size(
        volume_bar,
        136,
        14
    );

    lv_bar_set_range(
        volume_bar,
        0,
        100
    );

    lv_bar_set_value(
        volume_bar,
        VOLUME_PLACEHOLDER,
        LV_ANIM_OFF
    );

    lv_obj_set_style_bg_color(
        volume_bar,
        lv_color_hex(COLOR_WHITE),
        LV_PART_MAIN
    );

    lv_obj_set_style_bg_opa(
        volume_bar,
        LV_OPA_COVER,
        LV_PART_MAIN
    );

    lv_obj_set_style_radius(
        volume_bar,
        7,
        LV_PART_MAIN
    );

    lv_obj_set_style_bg_color(
        volume_bar,
        lv_color_hex(COLOR_TRACK),
        LV_PART_INDICATOR
    );

    lv_obj_set_style_bg_opa(
        volume_bar,
        LV_OPA_COVER,
        LV_PART_INDICATOR
    );

    lv_obj_set_style_radius(
        volume_bar,
        7,
        LV_PART_INDICATOR
    );

    volume_label =
        lv_label_create(screen);

    lv_label_set_text(
        volume_label,
        "36%"
    );

    lv_obj_set_pos(
        volume_label,
        174,
        201
    );

    lv_obj_set_size(
        volume_label,
        60,
        34
    );

    lv_obj_set_style_text_align(
        volume_label,
        LV_TEXT_ALIGN_LEFT,
        LV_PART_MAIN
    );

    lv_obj_set_style_text_color(
        volume_label,
        lv_color_hex(COLOR_WHITE),
        LV_PART_MAIN
    );

    lv_obj_set_style_text_font(
        volume_label,
        &lv_font_montserrat_24,
        LV_PART_MAIN
    );

    update_layer();
    update_rgb();
    update_battery();

    lv_timer_create(
        update_ui,
        UI_UPDATE_PERIOD_MS,
        NULL
    );

    return screen;
}
