/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_kscan_adc_joystick

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/kscan.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define JOYSTICK_COLUMNS 4
#define JOY_COL_UP 0
#define JOY_COL_DOWN 1
#define JOY_COL_LEFT 2
#define JOY_COL_RIGHT 3

#define BUTTONS_LEN(n) DT_INST_PROP_LEN(n, button_gpios)
#define TOTAL_COLUMNS(n) (DT_INST_PROP(n, button_column_offset) + BUTTONS_LEN(n))

struct kscan_adc_joystick_config {
    struct adc_dt_spec adc_x;
    struct adc_dt_spec adc_y;
    const struct gpio_dt_spec *buttons;
    uint8_t button_count;
    uint8_t button_column_offset;
    uint16_t poll_period_ms;
    uint16_t center;
    uint16_t deadzone;
    uint16_t hysteresis;
    bool invert_x;
    bool invert_y;
};

struct kscan_adc_joystick_data {
    const struct device *dev;
    kscan_callback_t callback;
    struct k_work_delayable work;
    uint32_t state_mask;
    int8_t axis_x;
    int8_t axis_y;
    bool enabled;
};

static bool gpio_is_pressed(const struct gpio_dt_spec *gpio) {
    int value = gpio_pin_get_dt(gpio);
    if (value < 0) {
        return false;
    }
    return value > 0;
}

static int adc_read_raw(const struct adc_dt_spec *spec, int32_t *out_raw) {
    int16_t raw = 0;
    struct adc_sequence sequence = {0};

    int ret = adc_sequence_init_dt(spec, &sequence);
    if (ret < 0) {
        return ret;
    }
    sequence.buffer = &raw;
    sequence.buffer_size = sizeof(raw);

    ret = adc_read_dt(spec, &sequence);
    if (ret < 0) {
        return ret;
    }

    *out_raw = raw;
    return 0;
}

static int8_t axis_from_sample(int32_t sample, int8_t prev_axis,
                               const struct kscan_adc_joystick_config *cfg) {
    const int32_t low_on = (int32_t)cfg->center - ((int32_t)cfg->deadzone + cfg->hysteresis);
    const int32_t low_off = (int32_t)cfg->center - ((int32_t)cfg->deadzone - cfg->hysteresis);
    const int32_t high_on = (int32_t)cfg->center + ((int32_t)cfg->deadzone + cfg->hysteresis);
    const int32_t high_off = (int32_t)cfg->center + ((int32_t)cfg->deadzone - cfg->hysteresis);

    if (prev_axis < 0) {
        return (sample < low_off) ? -1 : 0;
    }

    if (prev_axis > 0) {
        return (sample > high_off) ? 1 : 0;
    }

    if (sample < low_on) {
        return -1;
    }

    if (sample > high_on) {
        return 1;
    }

    return 0;
}

static void emit_changes(const struct device *dev, uint32_t old_mask, uint32_t new_mask,
                         uint8_t max_cols) {
    struct kscan_adc_joystick_data *data = dev->data;
    const uint32_t changed = old_mask ^ new_mask;

    if (data->callback == NULL || changed == 0U) {
        return;
    }

    for (uint8_t col = 0; col < max_cols; col++) {
        if ((changed & BIT(col)) == 0U) {
            continue;
        }

        const bool pressed = (new_mask & BIT(col)) != 0U;
        data->callback(dev, 0, col, pressed);
    }
}

static int kscan_adc_joystick_scan(const struct device *dev) {
    struct kscan_adc_joystick_data *data = dev->data;
    const struct kscan_adc_joystick_config *cfg = dev->config;

    int32_t x_raw = 0;
    int32_t y_raw = 0;
    int ret = adc_read_raw(&cfg->adc_x, &x_raw);
    if (ret < 0) {
        LOG_ERR("ADC X read failed: %d", ret);
        return ret;
    }

    ret = adc_read_raw(&cfg->adc_y, &y_raw);
    if (ret < 0) {
        LOG_ERR("ADC Y read failed: %d", ret);
        return ret;
    }

    int8_t axis_x = axis_from_sample(x_raw, data->axis_x, cfg);
    int8_t axis_y = axis_from_sample(y_raw, data->axis_y, cfg);

    if (cfg->invert_x) {
        axis_x = -axis_x;
    }

    if (cfg->invert_y) {
        axis_y = -axis_y;
    }

    uint32_t state_mask = 0U;

    if (axis_y < 0) {
        state_mask |= BIT(JOY_COL_UP);
    } else if (axis_y > 0) {
        state_mask |= BIT(JOY_COL_DOWN);
    }

    if (axis_x < 0) {
        state_mask |= BIT(JOY_COL_LEFT);
    } else if (axis_x > 0) {
        state_mask |= BIT(JOY_COL_RIGHT);
    }

    for (uint8_t i = 0; i < cfg->button_count; i++) {
        if (gpio_is_pressed(&cfg->buttons[i])) {
            state_mask |= BIT(cfg->button_column_offset + i);
        }
    }

    emit_changes(dev, data->state_mask, state_mask, cfg->button_column_offset + cfg->button_count);

    data->state_mask = state_mask;
    data->axis_x = axis_x;
    data->axis_y = axis_y;

    return 0;
}

static void kscan_adc_joystick_work(struct k_work *work) {
    struct k_work_delayable *dwork = CONTAINER_OF(work, struct k_work_delayable, work);
    struct kscan_adc_joystick_data *data =
        CONTAINER_OF(dwork, struct kscan_adc_joystick_data, work);
    const struct kscan_adc_joystick_config *cfg = data->dev->config;

    kscan_adc_joystick_scan(data->dev);

    if (data->enabled) {
        k_work_reschedule(&data->work, K_MSEC(cfg->poll_period_ms));
    }
}

static int kscan_adc_joystick_configure(const struct device *dev, kscan_callback_t callback) {
    struct kscan_adc_joystick_data *data = dev->data;

    if (callback == NULL) {
        return -EINVAL;
    }

    data->callback = callback;
    return 0;
}

static int kscan_adc_joystick_enable(const struct device *dev) {
    struct kscan_adc_joystick_data *data = dev->data;

    data->enabled = true;
    return k_work_reschedule(&data->work, K_NO_WAIT);
}

static int kscan_adc_joystick_disable(const struct device *dev) {
    struct kscan_adc_joystick_data *data = dev->data;

    data->enabled = false;
    return k_work_cancel_delayable(&data->work);
}

static int kscan_adc_joystick_init(const struct device *dev) {
    struct kscan_adc_joystick_data *data = dev->data;
    const struct kscan_adc_joystick_config *cfg = dev->config;

    data->dev = dev;
    data->callback = NULL;
    data->enabled = false;
    data->state_mask = 0U;
    data->axis_x = 0;
    data->axis_y = 0;
    k_work_init_delayable(&data->work, kscan_adc_joystick_work);

    if (!adc_is_ready_dt(&cfg->adc_x)) {
        LOG_ERR("ADC X device not ready");
        return -ENODEV;
    }

    if (!adc_is_ready_dt(&cfg->adc_y)) {
        LOG_ERR("ADC Y device not ready");
        return -ENODEV;
    }

    int ret = adc_channel_setup_dt(&cfg->adc_x);
    if (ret < 0) {
        LOG_ERR("ADC X channel setup failed: %d", ret);
        return ret;
    }

    ret = adc_channel_setup_dt(&cfg->adc_y);
    if (ret < 0) {
        LOG_ERR("ADC Y channel setup failed: %d", ret);
        return ret;
    }

    for (uint8_t i = 0; i < cfg->button_count; i++) {
        const struct gpio_dt_spec *button = &cfg->buttons[i];
        if (!device_is_ready(button->port)) {
            LOG_ERR("Button GPIO not ready: %s", button->port->name);
            return -ENODEV;
        }

        ret = gpio_pin_configure_dt(button, GPIO_INPUT);
        if (ret < 0) {
            LOG_ERR("Button GPIO configure failed (pin %u on %s): %d", button->pin,
                    button->port->name, ret);
            return ret;
        }
    }

    return 0;
}

static const struct kscan_driver_api kscan_adc_joystick_api = {
    .config = kscan_adc_joystick_configure,
    .enable_callback = kscan_adc_joystick_enable,
    .disable_callback = kscan_adc_joystick_disable,
};

#define BUTTON_SPEC(i, n) GPIO_DT_SPEC_INST_GET_BY_IDX(n, button_gpios, i)

#define KSCAN_ADC_JOYSTICK_INIT(n)                                                                 \
    BUILD_ASSERT(TOTAL_COLUMNS(n) <= 32,                                                           \
                 "zmk,kscan-adc-joystick supports max 32 columns");                                \
    BUILD_ASSERT(DT_INST_PROP(n, button_column_offset) >= JOYSTICK_COLUMNS,                        \
                 "button-column-offset must be >= 4");                                              \
    static const struct gpio_dt_spec kscan_adc_joystick_buttons_##n[] = {                          \
        LISTIFY(BUTTONS_LEN(n), BUTTON_SPEC, (, ), n)};                                             \
    static const struct kscan_adc_joystick_config kscan_adc_joystick_config_##n = {                \
        .adc_x = ADC_DT_SPEC_INST_GET_BY_IDX(n, 0),                                                 \
        .adc_y = ADC_DT_SPEC_INST_GET_BY_IDX(n, 1),                                                 \
        .buttons = kscan_adc_joystick_buttons_##n,                                                  \
        .button_count = BUTTONS_LEN(n),                                                             \
        .button_column_offset = DT_INST_PROP(n, button_column_offset),                              \
        .poll_period_ms = DT_INST_PROP(n, poll_period_ms),                                          \
        .center = DT_INST_PROP(n, center),                                                          \
        .deadzone = DT_INST_PROP(n, deadzone),                                                      \
        .hysteresis = DT_INST_PROP(n, hysteresis),                                                  \
        .invert_x = DT_INST_PROP(n, invert_x),                                                      \
        .invert_y = DT_INST_PROP(n, invert_y),                                                      \
    };                                                                                              \
    static struct kscan_adc_joystick_data kscan_adc_joystick_data_##n;                             \
    DEVICE_DT_INST_DEFINE(n, kscan_adc_joystick_init, NULL, &kscan_adc_joystick_data_##n,          \
                          &kscan_adc_joystick_config_##n, POST_KERNEL,                              \
                          CONFIG_KSCAN_INIT_PRIORITY, &kscan_adc_joystick_api);

DT_INST_FOREACH_STATUS_OKAY(KSCAN_ADC_JOYSTICK_INIT)
