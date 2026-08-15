/*
 * ClavisMacro Hall-effect KSCAN driver
 *
 * 9x HAL403 through CD74HC4067
 * XIAO nRF52840 Plus
 *
 * Ported from the proven Arduino HallKeys.h implementation.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT rvan_clavis_hall_kscan

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/kscan.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(clavis_hall, LOG_LEVEL_INF);

#define CLAVIS_HALL_KEY_COUNT 9

struct clavis_hall_key_state {
    int32_t rest;
    int32_t filtered;
    int32_t peak;
    int32_t valley;
    int32_t raw;
    bool pressed;
};

struct clavis_hall_config {
    struct adc_dt_spec adc;

    struct gpio_dt_spec mux_s0;
    struct gpio_dt_spec mux_s1;
    struct gpio_dt_spec mux_s2;
    struct gpio_dt_spec mux_s3;
    struct gpio_dt_spec mux_enable;

    uint16_t actuation_counts;
    uint16_t rapid_trigger_counts;
    uint16_t release_counts;

    uint8_t filter_shift;

    uint16_t scan_period_ms;
    uint16_t mux_settle_us;
};

struct clavis_hall_data {
    const struct device *dev;

    kscan_callback_t callback;

    struct k_work_delayable scan_work;

    struct clavis_hall_key_state keys[CLAVIS_HALL_KEY_COUNT];

    bool enabled;
};

static inline int32_t clavis_abs32(int32_t value) {
    return value < 0 ? -value : value;
}

static inline int32_t clavis_hall_travel(
    const struct clavis_hall_key_state *key
) {
    return clavis_abs32(key->filtered - key->rest);
}

static int clavis_mux_select(
    const struct clavis_hall_config *cfg,
    uint8_t channel
) {
    int err;

    err = gpio_pin_set_dt(
        &cfg->mux_s0,
        (channel & BIT(0)) != 0
    );

    if (err < 0) {
        return err;
    }

    err = gpio_pin_set_dt(
        &cfg->mux_s1,
        (channel & BIT(1)) != 0
    );

    if (err < 0) {
        return err;
    }

    err = gpio_pin_set_dt(
        &cfg->mux_s2,
        (channel & BIT(2)) != 0
    );

    if (err < 0) {
        return err;
    }

    err = gpio_pin_set_dt(
        &cfg->mux_s3,
        (channel & BIT(3)) != 0
    );

    if (err < 0) {
        return err;
    }

    return 0;
}

static int clavis_adc_sample(
    const struct clavis_hall_config *cfg,
    int32_t *value
) {
    int16_t sample = 0;

    struct adc_sequence sequence = {
        .buffer = &sample,
        .buffer_size = sizeof(sample),
    };

    int err = adc_sequence_init_dt(
        &cfg->adc,
        &sequence
    );

    if (err < 0) {
        return err;
    }

    err = adc_read_dt(
        &cfg->adc,
        &sequence
    );

    if (err < 0) {
        return err;
    }

    *value = sample;

    return 0;
}

static int clavis_hall_read_raw(
    const struct clavis_hall_config *cfg,
    uint8_t channel,
    int32_t *value
) {
    int err = clavis_mux_select(
        cfg,
        channel
    );

    if (err < 0) {
        return err;
    }

    /*
     * Let the CD74HC4067 + ADC sampling capacitor settle.
     */
    k_busy_wait(cfg->mux_settle_us);

    /*
     * Throw away first ADC conversion after MUX channel change.
     */
    int32_t dummy;

    err = clavis_adc_sample(
        cfg,
        &dummy
    );

    if (err < 0) {
        return err;
    }

    return clavis_adc_sample(
        cfg,
        value
    );
}

static int clavis_hall_calibrate(
    const struct device *dev
) {
    const struct clavis_hall_config *cfg =
        dev->config;

    struct clavis_hall_data *data =
        dev->data;

    /*
     * Give Hall sensors and analog path time to settle after boot.
     */
    k_sleep(K_MSEC(3000));

    for (
        uint8_t key = 0;
        key < CLAVIS_HALL_KEY_COUNT;
        key++
    ) {
        int32_t total = 0;

        for (
            uint8_t sample = 0;
            sample < 32;
            sample++
        ) {
            int32_t raw;

            int err = clavis_hall_read_raw(
                cfg,
                key,
                &raw
            );

            if (err < 0) {
                LOG_ERR(
                    "Calibration ADC error on key %u: %d",
                    key,
                    err
                );

                return err;
            }

            total += raw;

            k_busy_wait(100);
        }

        int32_t rest =
            total / 32;

        data->keys[key].rest =
            rest;

        data->keys[key].filtered =
            rest;

        data->keys[key].raw =
            rest;

        data->keys[key].peak =
            0;

        data->keys[key].valley =
            0;

        data->keys[key].pressed =
            false;
    }

    return 0;
}

static int clavis_hall_scan_once(
    const struct device *dev
) {
    const struct clavis_hall_config *cfg =
        dev->config;

    struct clavis_hall_data *data =
        dev->data;

    if (data->callback == NULL) {
        return 0;
    }

    for (
        uint8_t key_index = 0;
        key_index < CLAVIS_HALL_KEY_COUNT;
        key_index++
    ) {
        struct clavis_hall_key_state *key =
            &data->keys[key_index];

        int32_t raw;

        int err = clavis_hall_read_raw(
            cfg,
            key_index,
            &raw
        );

        if (err < 0) {
            return err;
        }

        key->raw = raw;

        /*
         * EMA filter:
         *
         * filtered +=
         *     (raw - filtered) >> filter_shift
         */
        key->filtered +=
            (key->raw - key->filtered)
            >> cfg->filter_shift;

        int32_t travel =
            clavis_hall_travel(key);

        if (!key->pressed) {
            /*
             * Track local valley after release.
             */
            if (travel < key->valley) {
                key->valley = travel;
            }

            int32_t dynamic_trigger =
                key->valley +
                cfg->rapid_trigger_counts;

            int32_t trigger =
                MAX(
                    (int32_t)cfg->actuation_counts,
                    dynamic_trigger
                );

            if (travel >= trigger) {
                key->pressed = true;
                key->peak = travel;

                uint32_t row =
                    key_index / 3;

                uint32_t column =
                    key_index % 3;

                data->callback(
                    dev,
                    row,
                    column,
                    true
                );
            }

        } else {
            /*
             * While pressed, track deepest position.
             */
            if (travel > key->peak) {
                key->peak = travel;
            }

            /*
             * Rapid Trigger release:
             *
             * 1. release when nearly back at rest
             *
             * OR
             *
             * 2. release after moving upward
             *    rapid_trigger_counts from peak.
             */
            if (
                travel <= cfg->release_counts ||
                travel <= (
                    key->peak -
                    cfg->rapid_trigger_counts
                )
            ) {
                key->pressed = false;
                key->valley = travel;

                uint32_t row =
                    key_index / 3;

                uint32_t column =
                    key_index % 3;

                data->callback(
                    dev,
                    row,
                    column,
                    false
                );
            }
        }
    }

    return 0;
}

static void clavis_hall_work_handler(
    struct k_work *work
) {
    struct k_work_delayable *dwork =
        k_work_delayable_from_work(work);

    struct clavis_hall_data *data =
        CONTAINER_OF(
            dwork,
            struct clavis_hall_data,
            scan_work
        );

    if (!data->enabled) {
        return;
    }

    const struct clavis_hall_config *cfg =
        data->dev->config;

    int err =
        clavis_hall_scan_once(
            data->dev
        );

    if (err < 0) {
        LOG_WRN(
            "Hall scan failed: %d",
            err
        );
    }

    if (data->enabled) {
        k_work_reschedule(
            &data->scan_work,
            K_MSEC(
                cfg->scan_period_ms
            )
        );
    }
}

static int clavis_hall_configure(
    const struct device *dev,
    kscan_callback_t callback
) {
    struct clavis_hall_data *data =
        dev->data;

    if (callback == NULL) {
        return -EINVAL;
    }

    data->callback =
        callback;

    return 0;
}

static int clavis_hall_enable(
    const struct device *dev
) {
    struct clavis_hall_data *data =
        dev->data;

    data->enabled = true;

    k_work_reschedule(
        &data->scan_work,
        K_NO_WAIT
    );

    return 0;
}

static int clavis_hall_disable(
    const struct device *dev
) {
    struct clavis_hall_data *data =
        dev->data;

    data->enabled =
        false;

    k_work_cancel_delayable(
        &data->scan_work
    );

    return 0;
}

static int clavis_hall_init_gpio(
    const struct gpio_dt_spec *gpio,
    gpio_flags_t flags
) {
    if (!device_is_ready(gpio->port)) {
        return -ENODEV;
    }

    return gpio_pin_configure_dt(
        gpio,
        flags
    );
}

static int clavis_hall_init(
    const struct device *dev
) {
    const struct clavis_hall_config *cfg =
        dev->config;

    struct clavis_hall_data *data =
        dev->data;

    data->dev =
        dev;

    data->enabled =
        false;

    data->callback =
        NULL;

    k_work_init_delayable(
        &data->scan_work,
        clavis_hall_work_handler
    );

    /*
     * Configure CD74HC4067 address pins.
     */
    int err =
        clavis_hall_init_gpio(
            &cfg->mux_s0,
            GPIO_OUTPUT_INACTIVE
        );

    if (err < 0) {
        return err;
    }

    err =
        clavis_hall_init_gpio(
            &cfg->mux_s1,
            GPIO_OUTPUT_INACTIVE
        );

    if (err < 0) {
        return err;
    }

    err =
        clavis_hall_init_gpio(
            &cfg->mux_s2,
            GPIO_OUTPUT_INACTIVE
        );

    if (err < 0) {
        return err;
    }

    err =
        clavis_hall_init_gpio(
            &cfg->mux_s3,
            GPIO_OUTPUT_INACTIVE
        );

    if (err < 0) {
        return err;
    }

    /*
     * CD74HC4067 EN is active LOW.
     */
    err =
        clavis_hall_init_gpio(
            &cfg->mux_enable,
            GPIO_OUTPUT_LOW
        );

    if (err < 0) {
        return err;
    }

    /*
     * Configure nRF52840 SAADC channel.
     */
    if (!adc_is_ready_dt(&cfg->adc)) {
        LOG_ERR(
            "Hall ADC device not ready"
        );

        return -ENODEV;
    }

    err =
        adc_channel_setup_dt(
            &cfg->adc
        );

    if (err < 0) {
        LOG_ERR(
            "Hall ADC channel setup failed: %d",
            err
        );

        return err;
    }

    err =
        clavis_hall_calibrate(
            dev
        );

    if (err < 0) {
        return err;
    }

    return 0;
}

static const struct kscan_driver_api
    clavis_hall_api = {
        .config =
            clavis_hall_configure,

        .enable_callback =
            clavis_hall_enable,

        .disable_callback =
            clavis_hall_disable,
};

#define CLAVIS_HALL_INIT(inst)                                      \
    static struct clavis_hall_data clavis_hall_data_##inst;        \
                                                                    \
    static const struct clavis_hall_config                          \
        clavis_hall_config_##inst = {                               \
            .adc = ADC_DT_SPEC_INST_GET(inst),                      \
                                                                    \
            .mux_s0 = GPIO_DT_SPEC_INST_GET(                        \
                inst, mux_s0_gpios),                                \
                                                                    \
            .mux_s1 = GPIO_DT_SPEC_INST_GET(                        \
                inst, mux_s1_gpios),                                \
                                                                    \
            .mux_s2 = GPIO_DT_SPEC_INST_GET(                        \
                inst, mux_s2_gpios),                                \
                                                                    \
            .mux_s3 = GPIO_DT_SPEC_INST_GET(                        \
                inst, mux_s3_gpios),                                \
                                                                    \
            .mux_enable = GPIO_DT_SPEC_INST_GET(                    \
                inst, mux_enable_gpios),                            \
                                                                    \
            .actuation_counts =                                     \
                DT_INST_PROP(inst, actuation_counts),               \
                                                                    \
            .rapid_trigger_counts =                                 \
                DT_INST_PROP(inst, rapid_trigger_counts),           \
                                                                    \
            .release_counts =                                       \
                DT_INST_PROP(inst, release_counts),                 \
                                                                    \
            .filter_shift =                                         \
                DT_INST_PROP(inst, filter_shift),                   \
                                                                    \
            .scan_period_ms =                                       \
                DT_INST_PROP(inst, scan_period_ms),                 \
                                                                    \
            .mux_settle_us =                                        \
                DT_INST_PROP(inst, mux_settle_us),                  \
        };                                                          \
                                                                    \
    DEVICE_DT_INST_DEFINE(                                          \
        inst,                                                       \
        clavis_hall_init,                                           \
        NULL,                                                       \
        &clavis_hall_data_##inst,                                   \
        &clavis_hall_config_##inst,                                 \
        POST_KERNEL,                                                \
        CONFIG_KSCAN_INIT_PRIORITY,                                 \
        &clavis_hall_api                                            \
    );

DT_INST_FOREACH_STATUS_OKAY(CLAVIS_HALL_INIT)