/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Experimental Core2 v1.1 (AXP2101 + INA3221), with Module LLM audio on UART2.
 * This is a separate board from the AXP192/v1.0 port. It has not been run on
 * hardware. No local microphone, speaker, vibration, SD, IMU or RTC driver.
 *
 * Verified vendor references (pins, LCD/touch and PMU register meanings):
 * https://github.com/espressif/esp-bsp/blob/f259a6abd5808019b1903a422092a4234ca95a51/bsp/m5stack_core_2/m5stack_core_2.c
 * https://github.com/espressif/esp-bsp/blob/f259a6abd5808019b1903a422092a4234ca95a51/bsp/m5stack_core_2/include/bsp/m5stack_core_2.h
 * https://github.com/m5stack/M5GFX/blob/a8ebd7bdaae2e9bdc2f3c69c25b83a84d819af72/src/M5GFX.cpp
 * https://github.com/m5stack/M5Unified/blob/35903583211e1f39ca88920714525fbcd1eb714a/src/utility/Power_Class.inl
 * https://github.com/m5stack/M5Unified/blob/35903583211e1f39ca88920714525fbcd1eb714a/src/utility/power/INA3221_Class.inl
 *
 * ALDO2 is LCD/touch reset, ALDO4 powers LCD/touch/SD, BLDO1 is backlight.
 * BLDO2 controls the external 5 V boost/bus switch: preserve its state.
 * In particular, do not call bsp_display_start_with_config() or the BSP's
 * brightness initializer: those also enable unrelated ALDO/BLDO rails.
 * The lower-level BSP panel/touch constructors only set ALDO4's voltage.
 * This uses the BSP's ILI9342C-compatible panel initialization. M5GFX also
 * documents later ILI9342E panels with a different init list: panel variant
 * and visible output must be checked on the actual unit before release.
 * The module keeps its existing external power arrangement. Charger, DCDC
 * voltages/protection and the INA3221 configuration are left untouched.
 */

#include "sdkconfig.h"
#if !CONFIG_BSP_PMU_AXP2101
#error "Core2 v1.1 requires the AXP2101 BSP option; never use the AXP192 map"
#endif

#include "esp_lcd_touch.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_board.h"
#include "muse_input.h"
#include "core2_strip.h"
#include "muse_mem.h"
#include "muse_pmu.h"

#define DRAW_BUF_LINES 10
#define AXP_ADDR 0x34
#define AXP_CHIP_ID 0x03
#define AXP_COMMON_CFG 0x10
#define AXP_BAT_DETECT 0x68
#define AXP_LDO_ENABLE 0x90
#define AXP_ALDO4_VOLTAGE 0x95
#define AXP_BLDO1_VOLTAGE 0x96
#define ALDO2_RESET (1u << 1)
#define ALDO3_SPEAKER (1u << 2)
#define ALDO4_LCD (1u << 3)
#define BLDO1_BACKLIGHT (1u << 4)
#define DLDO1_VIBRATION (1u << 7)
#define INA_ADDR 0x40
#define TP_ADDR 0x38
#define TP_Y_ZONE 240
#define TP_X_THIRD 107
#define PMU_KEY_EVERY 2

static const char *TAG = "core2_v11";
static i2c_master_dev_handle_t s_axp, s_tp;
static core2_strip_t s_strip;
static lv_indev_read_cb_t s_touch_read;
static lv_point_t s_touch_point;

static esp_err_t axp_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_axp, &reg, 1, value, 1, 100);
}

/* A failed read must never become a write of zero to a power-rail register. */
static esp_err_t axp_update(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t old;
    ESP_RETURN_ON_ERROR(axp_read(reg, &old), TAG, "PMU read 0x%02x", reg);
    uint8_t data[] = { reg, (old & (uint8_t)~mask) | (value & mask) };
    return i2c_master_transmit(s_axp, data, sizeof(data), 100);
}

static esp_err_t backlight(int pct)
{
    if (pct <= 0) {
        return axp_update(AXP_LDO_ENABLE, BLDO1_BACKLIGHT, 0);
    }
    if (pct > 100) pct = 100;
    /* Espressif's Core2 BSP uses BLDO1 at 2.5--3.3 V (100 mV steps).
     * Set the voltage before enabling it. Zero percent cuts only BLDO1. */
    ESP_RETURN_ON_ERROR(axp_update(AXP_BLDO1_VOLTAGE, 0x1f,
                                  20 + 8 * pct / 100), TAG, "backlight voltage");
    return axp_update(AXP_LDO_ENABLE, BLDO1_BACKLIGHT, BLDO1_BACKLIGHT);
}

/* Read-only identity check. M5Unified identifies INA3221 by register FF=3220.
 * Muse's battery UI uses AXP2101; do not reconfigure the external monitor or
 * infer USB/battery state from a channel whose wiring has not been measured. */
static void probe_ina3221(i2c_master_bus_handle_t bus)
{
    if (i2c_master_probe(bus, INA_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "INA3221 not responding; AXP2101 battery status remains available");
        return;
    }
    i2c_master_dev_handle_t dev = NULL;
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = INA_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &dev) != ESP_OK) return;
    uint8_t reg = 0xff, id[2];
    if (i2c_master_transmit_receive(dev, &reg, 1, id, sizeof(id), 100) == ESP_OK &&
        id[0] == 0x32 && id[1] == 0x20) {
        ESP_LOGI(TAG, "INA3221 identified; monitor configuration preserved");
    } else {
        ESP_LOGW(TAG, "unexpected monitor at 0x40; no monitor writes performed");
    }
    i2c_master_bus_rm_device(dev);
}

/* FT6336 BtnB is the middle third of the strip below the visible LCD.
 * -1 preserves the previous held state on a transient I2C read failure. */
static int tp_zone(void)
{
    uint8_t reg = 0x02, data[5];
    if (i2c_master_transmit_receive(s_tp, &reg, 1, data, sizeof(data), 50) != ESP_OK) {
        return -1;
    }
    if (!(data[0] & 0x0f)) return 0;
    int x = ((data[1] & 0x0f) << 8) | data[2];
    int y = ((data[3] & 0x0f) << 8) | data[4];
    if(y<TP_Y_ZONE || x<0 || x>=BSP_LCD_H_RES)return 0;
    return x<TP_X_THIRD?1:x<2*TP_X_THIRD?2:3;
}

static esp_err_t init(void)
{
    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "I2C init");
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    const i2c_device_config_t axp_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &axp_cfg, &s_axp), TAG, "PMU device");
    uint8_t id;
    ESP_RETURN_ON_ERROR(axp_read(AXP_CHIP_ID, &id), TAG, "PMU identity");
    ESP_RETURN_ON_FALSE(id == 0x4a, ESP_ERR_NOT_SUPPORTED, TAG,
                        "expected Core2 v1.1 AXP2101 (0x4a), read 0x%02x; no rail writes", id);

    /* Only verified LCD/reset rails are enabled. Leave BLDO2/5 V alone;
     * local speaker and vibration remain off with module-only audio. */
    ESP_RETURN_ON_ERROR(axp_update(AXP_ALDO4_VOLTAGE, 0x1f, 0x1c), TAG, "LCD 3.3 V");
    ESP_RETURN_ON_ERROR(axp_update(AXP_LDO_ENABLE,
                                  ALDO4_LCD | ALDO3_SPEAKER | BLDO1_BACKLIGHT | DLDO1_VIBRATION,
                                  ALDO4_LCD), TAG, "LCD power");
    /* M5Unified enables internal off-discharge (bits 5:4), which lets ALDO2
     * really fall during reset. Preserve every other common-config bit. */
    ESP_RETURN_ON_ERROR(axp_update(AXP_COMMON_CFG, 0x30, 0x30), TAG, "LDO discharge");
    ESP_RETURN_ON_ERROR(axp_update(AXP_LDO_ENABLE, ALDO2_RESET, 0), TAG, "LCD reset low");
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_RETURN_ON_ERROR(axp_update(AXP_LDO_ENABLE, ALDO2_RESET, ALDO2_RESET), TAG, "LCD reset high");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(axp_update(AXP_BAT_DETECT, 0x01, 0x01), TAG, "battery detection");
    ESP_RETURN_ON_ERROR(muse_pmu_init(bus, true), TAG, "PMU key and battery");
    probe_ina3221(bus);

    const i2c_device_config_t tp_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TP_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &tp_cfg, &s_tp), TAG, "touch device");
    int boot_zone=tp_zone();if(boot_zone<0)boot_zone=0;
    s_strip=(core2_strip_t){.stable=boot_zone,.candidate=boot_zone,.samples=3,.side_latched=boot_zone!=0};
    ESP_LOGI(TAG, "Core2 v1.1 ready; local audio disabled, external 5 V state preserved");
    return ESP_OK;
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    s_touch_read(indev, data);
    if (data->point.x < 0 || data->point.x >= BSP_LCD_H_RES ||
        data->point.y < 0 || data->point.y >= BSP_LCD_V_RES) {
        data->state = LV_INDEV_STATE_RELEASED;
        data->point = s_touch_point;
    } else {
        s_touch_point = data->point;
    }
}

static lv_display_t *display_start(lv_indev_t **touch)
{
    lvgl_port_cfg_t cfg = ESP_LVGL_PORT_INIT_CONFIG();
    cfg.task_affinity = MUSE_UI_CORE;
    cfg.task_priority = MUSE_UI_PRIORITY;
    cfg.task_max_sleep_ms = 10;
    if (lvgl_port_init(&cfg) != ESP_OK) return NULL;

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    const bsp_display_config_t panel_cfg = {
        .max_transfer_sz = BSP_LCD_H_RES * DRAW_BUF_LINES * sizeof(uint16_t),
    };
    if (bsp_display_new(&panel_cfg, &panel, &io) != ESP_OK ||
        esp_lcd_panel_disp_on_off(panel, true) != ESP_OK) return NULL;
    /* Classic ESP32 cannot DMA from PSRAM: two small internal buffers. */
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = BSP_LCD_H_RES * DRAW_BUF_LINES,
        .double_buffer = true,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_dma = true, .buff_spiram = false, .swap_bytes = true },
    };
    lv_display_t *disp = lvgl_port_add_disp(&display_cfg);
    if (!disp) return NULL;
    esp_lcd_touch_handle_t tp = NULL;
    if (bsp_touch_new(NULL, &tp) != ESP_OK) return NULL;
    const lvgl_port_touch_cfg_t touch_cfg = { .disp = disp, .handle = tp };
    *touch = lvgl_port_add_touch(&touch_cfg);
    if (!*touch) return NULL;
    lvgl_port_lock(0);
    s_touch_read = lv_indev_get_read_cb(*touch);
    lv_indev_set_read_cb(*touch, touch_read);
    /* The bottom PTT poll also reads this controller. Do not rely solely on
     * an interrupt edge surviving a busy narration redraw. Poll releases too. */
    lv_indev_set_mode(*touch,LV_INDEV_MODE_TIMER);
    lv_timer_set_period(lv_indev_get_read_timer(*touch),20);
    lvgl_port_unlock();
    return disp;
}

static bool display_lock(int timeout_ms)
{
    return lvgl_port_lock(timeout_ms < 0 ? 0 : (uint32_t)timeout_ms);
}

static void set_brightness(int pct)
{
    esp_err_t err = backlight(pct);
    if (err != ESP_OK) ESP_LOGW(TAG, "backlight: %s", esp_err_to_name(err));
}

static unsigned poll_buttons(void)
{
    unsigned out = 0;
    out|=core2_strip_update(&s_strip,tp_zone());
    static unsigned tick;
    if (tick++ % PMU_KEY_EVERY == 0) {
        unsigned key = muse_pmu_poll_key();
        if (key & MUSE_PMU_KEY_LONG) {
            muse_input_request_power_off();
        } else if (key & MUSE_PMU_KEY_CLICK) {
            out |= MUSE_BTN_AUX_PRESS | MUSE_BTN_AUX_RELEASE;
        }
    }
    return out;
}

static esp_err_t power_off(void)
{
    ESP_RETURN_ON_ERROR(backlight(0), TAG, "backlight off");
    return muse_pmu_power_off();
}

static const muse_board_t s_board = {
    .name = "M5Stack Core2 v1.1",
    .width = BSP_LCD_H_RES,
    .height = BSP_LCD_V_RES,
    .touch = true,
    .diagonal_in = 2.0f,
    .talk_button = "middle",
    .aux_button = "side",
    .talk_hint = { LV_ALIGN_BOTTOM_MID, 0, -30 },
    .aux_hint = { LV_ALIGN_LEFT_MID, 4, -98 },
    .frame_ms = 16,
    .init = init,
    .display_start = display_start,
    .display_lock = display_lock,
    .display_unlock = lvgl_port_unlock,
    .set_brightness = set_brightness,
    /* Module LLM owns mic/speaker; GPIO14 TX and GPIO13 RX stay free here. */
    .poll_buttons = poll_buttons,
    .read_power = muse_pmu_read_power,
    .power_off = power_off,
};

const muse_board_t *muse_board_get(void)
{
    return &s_board;
}
