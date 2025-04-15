#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"


// Camera配置
#define BSP_CAMERA_XCLK     15
#define BSP_CAMERA_PCLK     13
#define BSP_CAMERA_VSYNC    6
#define BSP_CAMERA_HSYNC    7
#define BSP_CAMERA_D0       11
#define BSP_CAMERA_D1       9
#define BSP_CAMERA_D2       8
#define BSP_CAMERA_D3       10
#define BSP_CAMERA_D4       12
#define BSP_CAMERA_D5       18
#define BSP_CAMERA_D6       17
#define BSP_CAMERA_D7       16
static i2c_master_bus_handle_t i2c_handle = NULL;
static bool i2c_initialized = false;
// I2C配置
#define BSP_I2C_SCL (GPIO_NUM_5)
#define BSP_I2C_SDA (GPIO_NUM_4)
#define BSP_I2C_NUM        I2C_NUM_0
#define BSP_I2C_FREQ       400000

// Camera默认配置
#define BSP_CAMERA_DEFAULT_CONFIG {                \
    .pin_pwdn = -1,                               \
    .pin_reset = -1,                              \
    .pin_xclk = BSP_CAMERA_XCLK,                 \
    .pin_sccb_sda = BSP_I2C_SDA,                 \
    .pin_sccb_scl = BSP_I2C_SCL,                 \
    .pin_d7 = BSP_CAMERA_D7,                     \
    .pin_d6 = BSP_CAMERA_D6,                     \
    .pin_d5 = BSP_CAMERA_D5,                     \
    .pin_d4 = BSP_CAMERA_D4,                     \
    .pin_d3 = BSP_CAMERA_D3,                     \
    .pin_d2 = BSP_CAMERA_D2,                     \
    .pin_d1 = BSP_CAMERA_D1,                     \
    .pin_d0 = BSP_CAMERA_D0,                     \
    .pin_vsync = BSP_CAMERA_VSYNC,               \
    .pin_href = BSP_CAMERA_HSYNC,                \
    .pin_pclk = BSP_CAMERA_PCLK,                 \
    .xclk_freq_hz = 16000000,                    \
    .ledc_timer = LEDC_TIMER_0,                  \
    .ledc_channel = LEDC_CHANNEL_0,              \
    .pixel_format = PIXFORMAT_RGB565,            \
    .frame_size = FRAMESIZE_240X240,             \
    .jpeg_quality = 12,                          \
    .fb_count = 2,                               \
    .fb_location = CAMERA_FB_IN_PSRAM,           \
    .sccb_i2c_port = BSP_I2C_NUM,                 \
}

#define BSP_CAMERA_VFLIP    1
#define BSP_CAMERA_HMIRROR  0

// I2C初始化函数
static esp_err_t bsp_i2c_init(void)
{
    /* I2C was initialized before */
    if (i2c_initialized) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t i2c_config = {
        .i2c_port = BSP_I2C_NUM,
        .sda_io_num = BSP_I2C_SDA, 
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    i2c_new_master_bus(&i2c_config, &i2c_handle);  

    i2c_initialized = true;
    return ESP_OK;
}

static esp_err_t bsp_i2c_deinit(void)
{
    i2c_del_master_bus(i2c_handle);
    i2c_initialized = false;
    return ESP_OK;
}