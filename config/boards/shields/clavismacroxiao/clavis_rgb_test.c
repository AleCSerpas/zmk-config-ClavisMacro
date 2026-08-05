#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(clavis_rgb_test, LOG_LEVEL_INF);

#define STRIP_NODE DT_CHOSEN(zmk_underglow)
#define LED_COUNT 10
#define UPDATE_DELAY_MS 100

static const struct device *const strip =
    DEVICE_DT_GET(STRIP_NODE);

static struct led_rgb pixels[LED_COUNT];
static struct k_work_delayable rgb_work;

static uint8_t position;

static void clear_pixels(void)
{
    for (int i = 0; i < LED_COUNT; i++) {
        pixels[i].r = 0;
        pixels[i].g = 0;
        pixels[i].b = 0;
    }
}

static void rgb_test_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    clear_pixels();

    /*
     * Un punto rojo se mueve LED por LED.
     * El LED opuesto se enciende azul.
     */
    pixels[position].r = 40;

    uint8_t opposite = (position + (LED_COUNT / 2)) % LED_COUNT;
    pixels[opposite].b = 40;

    int err = led_strip_update_rgb(strip, pixels, LED_COUNT);

    if (err != 0) {
        LOG_ERR("led_strip_update_rgb failed: %d", err);
    } else {
        LOG_INF("frame sent, position=%u", position);
    }

    position = (position + 1) % LED_COUNT;

    k_work_schedule(&rgb_work, K_MSEC(UPDATE_DELAY_MS));
}

static int clavis_rgb_test_init(void)
{
    if (!device_is_ready(strip)) {
        LOG_ERR("RGB strip device is not ready");
        return -ENODEV;
    }

    LOG_INF("RGB strip ready, length=%u",
            (unsigned int)led_strip_length(strip));

    k_work_init_delayable(&rgb_work, rgb_test_work_handler);
    k_work_schedule(&rgb_work, K_MSEC(500));

    return 0;
}

SYS_INIT(clavis_rgb_test_init, APPLICATION, 90);