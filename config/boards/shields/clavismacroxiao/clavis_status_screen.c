/*
 * ClavisMacro custom 240x240 status screen.
 *
 * Current data:
 *   - Active ZMK layer
 *   - RGB underglow hue and brightness
 *   - Battery percentage; hidden when ZMK reports 0%
 *
 * Placeholder:
 *   - Volume bar remains fixed at 36% for now because ordinary HID
 *     volume controls do not receive the host's real volume percentage.
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
#define COLOR_TRACK      0x777777
#define COLOR_SHADOW     0x000000

static lv_obj_t *layer_label;

static lv_obj_t *rgb_arc;
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
    lv_obj_set_size(arc, 50, 50);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_SCROLLABLE);

    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_rotation(arc, 270);
    lv_arc_set_bg_angles(arc, 0, 360);

    lv_obj_set_style_arc_width(arc, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_set_style_arc_width(arc, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(COLOR_WHITE), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);

    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);

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

static void update_layer(void) {
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    const char *name = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index));

    char text[20] = {};

    if (name != NULL && strlen(name) > 0) {
        snprintf(text, sizeof(text), "%s", name);
    } else {
        snprintf(text, sizeof(text), "Layer %u", (unsigned int)index);
    }

    lv_label_set_text(layer_label, text);
}

static void update_rgb(void) {
    struct clavis_rgb_state rgb_state = {0};

    if (clavis_rgb_get_state(&rgb_state) < 0) {
        lv_arc_set_value(rgb_arc, 0);
        lv_label_set_text(rgb_label, "0%");
        lv_obj_set_style_arc_color(
            rgb_arc,
            lv_color_hex(COLOR_TRACK),
            LV_PART_INDICATOR
        );
        return;
    }

    uint8_t brightness = rgb_state.on ? CLAMP(rgb_state.brightness, 0, 100) : 0;

    char text[8] = {};
    snprintf(text, sizeof(text), "%u%%", brightness);

    lv_arc_set_value(rgb_arc, brightness);
    lv_label_set_text(rgb_label, text);

    if (rgb_state.on) {
        lv_color_t rgb_color =
            lv_color_hsv_to_rgb(
                rgb_state.hue % 360,
                rgb_state.saturation,
                100
            );

        lv_obj_set_style_arc_color(
            rgb_arc,
            rgb_color,
            LV_PART_INDICATOR
        );
    } else {
        lv_obj_set_style_arc_color(
            rgb_arc,
            lv_color_hex(COLOR_TRACK),
            LV_PART_INDICATOR
        );
    }
}

static void update_battery(void) {
    uint8_t level = CLAMP(zmk_battery_state_of_charge(), 0, 100);

    /*
     * On this first UI revision, a 0% reading is treated as no battery and
     * hides the entire battery widget. We can replace this with a dedicated
     * battery-presence test later if needed.
     */
    if (level == 0) {
        lv_obj_add_flag(battery_group, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_remove_flag(battery_group, LV_OBJ_FLAG_HIDDEN);

    char text[8] = {};
    snprintf(text, sizeof(text), "%u%%", level);

    lv_arc_set_value(battery_arc, level);
    lv_label_set_text(battery_label, text);
}

static void update_ui(lv_timer_t *timer) {
    ARG_UNUSED(timer);

    update_layer();
    update_rgb();
    update_battery();
}

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);

    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, 240, 240);
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BACKGROUND), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    /* RGB brightness ring — top-left of the two rings. */
    rgb_arc = create_ring(screen, 126, 8);
    rgb_label = create_ring_label(screen, 126, 23);

    /* Battery ring — top-right. */
    battery_group = lv_obj_create(screen);
    lv_obj_remove_style_all(battery_group);
    lv_obj_set_pos(battery_group, 182, 8);
    lv_obj_set_size(battery_group, 50, 50);
    lv_obj_remove_flag(battery_group, LV_OBJ_FLAG_SCROLLABLE);

    battery_arc = create_ring(battery_group, 0, 0);
    battery_label = create_ring_label(battery_group, 0, 15);
    lv_obj_set_style_arc_color(battery_arc, lv_color_hex(COLOR_WHITE), LV_PART_INDICATOR);

    /* Central rounded layer panel. */
    lv_obj_t *layer_panel = lv_obj_create(screen);
    lv_obj_remove_style_all(layer_panel);
    lv_obj_set_pos(layer_panel, 43, 73);
    lv_obj_set_size(layer_panel, 154, 96);
    lv_obj_set_style_bg_color(layer_panel, lv_color_hex(COLOR_PANEL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(layer_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(layer_panel, lv_color_hex(COLOR_WHITE), LV_PART_MAIN);
    lv_obj_set_style_border_width(layer_panel, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(layer_panel, 24, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(layer_panel, lv_color_hex(COLOR_SHADOW), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(layer_panel, 10, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(layer_panel, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_shadow_offset_x(layer_panel, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_offset_y(layer_panel, 5, LV_PART_MAIN);
    lv_obj_remove_flag(layer_panel, LV_OBJ_FLAG_SCROLLABLE);

    layer_label = lv_label_create(layer_panel);
    lv_obj_set_style_text_color(layer_label, lv_color_hex(COLOR_WHITE), LV_PART_MAIN);
    lv_obj_set_style_text_font(layer_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_align(layer_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(layer_label);

    /* Volume placeholder — fixed at 36% until host feedback is added. */
    volume_bar = lv_bar_create(screen);
    lv_obj_remove_style_all(volume_bar);
    lv_obj_set_pos(volume_bar, 28, 211);
    lv_obj_set_size(volume_bar, 136, 14);
    lv_bar_set_range(volume_bar, 0, 100);
    lv_bar_set_value(volume_bar, VOLUME_PLACEHOLDER, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(volume_bar, lv_color_hex(COLOR_WHITE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(volume_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(volume_bar, 7, LV_PART_MAIN);

    lv_obj_set_style_bg_color(volume_bar, lv_color_hex(COLOR_TRACK), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(volume_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(volume_bar, 7, LV_PART_INDICATOR);

    volume_label = lv_label_create(screen);
    lv_label_set_text(volume_label, "36%");
    lv_obj_set_pos(volume_label, 174, 201);
    lv_obj_set_size(volume_label, 60, 34);
    lv_obj_set_style_text_align(volume_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
    lv_obj_set_style_text_color(volume_label, lv_color_hex(COLOR_WHITE), LV_PART_MAIN);
    lv_obj_set_style_text_font(volume_label, &lv_font_montserrat_24, LV_PART_MAIN);

    update_layer();
    update_rgb();
    update_battery();

    lv_timer_create(update_ui, UI_UPDATE_PERIOD_MS, NULL);

    return screen;
}
