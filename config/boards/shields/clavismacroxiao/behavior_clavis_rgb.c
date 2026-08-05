/*
 * ZMK behavior bridge for the ClavisMacro RGB Matrix engine.
 *
 * Keymaps can use the normal ZMK RGB command constants, but bind them to
 * &cl_rgb instead of &rgb_ug.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_clavis_rgb

#include "clavis_rgb_engine.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <dt-bindings/zmk/rgb.h>
#include <zmk/behavior.h>

LOG_MODULE_REGISTER(clavis_rgb_behavior, LOG_LEVEL_INF);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    switch (binding->param1) {
    case RGB_TOG_CMD:
        return clavis_rgb_toggle();
    case RGB_ON_CMD:
        return clavis_rgb_on();
    case RGB_OFF_CMD:
        return clavis_rgb_off();
    case RGB_HUI_CMD:
        return clavis_rgb_change_hue(1);
    case RGB_HUD_CMD:
        return clavis_rgb_change_hue(-1);
    case RGB_SAI_CMD:
        return clavis_rgb_change_saturation(1);
    case RGB_SAD_CMD:
        return clavis_rgb_change_saturation(-1);
    case RGB_BRI_CMD:
        return clavis_rgb_change_brightness(1);
    case RGB_BRD_CMD:
        return clavis_rgb_change_brightness(-1);
    case RGB_SPI_CMD:
        return clavis_rgb_change_speed(1);
    case RGB_SPD_CMD:
        return clavis_rgb_change_speed(-1);
    case RGB_EFF_CMD:
        return clavis_rgb_cycle_effect(1);
    case RGB_EFR_CMD:
        return clavis_rgb_cycle_effect(-1);
    case RGB_EFS_CMD:
        return clavis_rgb_select_effect((uint8_t)binding->param2);
    case RGB_COLOR_HSB_CMD:
        return clavis_rgb_set_hsb((uint16_t)((binding->param2 >> 16) & 0xFFFFU),
                                  (uint8_t)((binding->param2 >> 8) & 0xFFU),
                                  (uint8_t)(binding->param2 & 0xFFU));
    default:
        LOG_WRN("Unsupported Clavis RGB command: %u", binding->param1);
        return -ENOTSUP;
    }
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_clavis_rgb_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_clavis_rgb_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
