/*
 * Rvan ClavisMacro - Runtime screen configuration
 *
 * Settings are persisted in NVS under:
 *   clavis/screen/state
 *
 * SPDX-License-Identifier: MIT
 */

#include "clavis_screen_config.h"

#include <errno.h>
#include <stdbool.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#define CLAVIS_SCREEN_SETTINGS_MAGIC 0x43534352U /* "CSCR" */
#define CLAVIS_SCREEN_SETTINGS_VERSION 1U

#ifndef CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE
#define CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE 500
#endif

struct clavis_screen_persisted_state {
    uint32_t magic;
    uint8_t version;
    uint8_t flags;
};

static struct clavis_screen_state state = {
    .flags = CLAVIS_SCREEN_DEFAULT_FLAGS,
};

static struct k_mutex state_lock;
static bool screen_config_ready;

#if IS_ENABLED(CONFIG_SETTINGS)
static struct k_work_delayable save_work;
#endif

#if IS_ENABLED(CONFIG_SETTINGS)
static void save_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    struct clavis_screen_state snapshot;

    k_mutex_lock(&state_lock, K_FOREVER);
    snapshot = state;
    k_mutex_unlock(&state_lock);

    const struct clavis_screen_persisted_state persisted = {
        .magic = CLAVIS_SCREEN_SETTINGS_MAGIC,
        .version = CLAVIS_SCREEN_SETTINGS_VERSION,
        .flags = snapshot.flags,
    };

    settings_save_one(
        "clavis/screen/state",
        &persisted,
        sizeof(persisted)
    );
}

static void schedule_save(void) {
    if (screen_config_ready) {
        k_work_reschedule(
            &save_work,
            K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE)
        );
    }
}
#else
static void schedule_save(void) {}
#endif

#if IS_ENABLED(CONFIG_SETTINGS)
static int settings_set(const char *name,
                        size_t len,
                        settings_read_cb read_cb,
                        void *cb_arg) {
    const char *next;

    if (!settings_name_steq(name, "state", &next) ||
        next != NULL) {
        return -ENOENT;
    }

    if (len != sizeof(struct clavis_screen_persisted_state)) {
        return -EINVAL;
    }

    struct clavis_screen_persisted_state persisted;

    int rc = read_cb(
        cb_arg,
        &persisted,
        sizeof(persisted)
    );

    if (rc < 0) {
        return rc;
    }

    if (persisted.magic != CLAVIS_SCREEN_SETTINGS_MAGIC ||
        persisted.version != CLAVIS_SCREEN_SETTINGS_VERSION ||
        (persisted.flags & ~CLAVIS_SCREEN_ALLOWED_FLAGS) != 0U) {
        return -EINVAL;
    }

    if (screen_config_ready) {
        k_mutex_lock(&state_lock, K_FOREVER);
        state.flags = persisted.flags;
        k_mutex_unlock(&state_lock);
    } else {
        state.flags = persisted.flags;
    }

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(
    clavis_screen_settings,
    "clavis/screen",
    NULL,
    settings_set,
    NULL,
    NULL
);
#endif

static int clavis_screen_config_init(void) {
    k_mutex_init(&state_lock);

#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_init_delayable(
        &save_work,
        save_work_handler
    );
#endif

    screen_config_ready = true;
    return 0;
}

SYS_INIT(
    clavis_screen_config_init,
    APPLICATION,
    88
);

int clavis_screen_get_state(struct clavis_screen_state *out_state) {
    if (out_state == NULL) {
        return -EINVAL;
    }

    k_mutex_lock(&state_lock, K_FOREVER);
    *out_state = state;
    k_mutex_unlock(&state_lock);

    return 0;
}

int clavis_screen_set_flags(uint8_t flags) {
    if ((flags & ~CLAVIS_SCREEN_ALLOWED_FLAGS) != 0U) {
        return -EINVAL;
    }

    k_mutex_lock(&state_lock, K_FOREVER);
    state.flags = flags;
    k_mutex_unlock(&state_lock);

    schedule_save();
    return 0;
}
