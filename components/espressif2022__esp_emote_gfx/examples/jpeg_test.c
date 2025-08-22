/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gfx.h"
#include "jpeg_display_example.h"
#include "esp_log.h"

static const char *TAG = "jpeg_test";

// Example JPEG data (you would replace this with your actual JPEG data)
// This is just a placeholder - in real use, this would be actual JPEG bytes
static const uint8_t example_jpeg_data[] = {
    0xFF, 0xD8, 0xFF, 0xE0, // JPEG SOI + APP0
    // ... rest of JPEG data would go here
};
static const uint32_t example_jpeg_size = sizeof(example_jpeg_data);

/**
 * @brief Test JPEG display functionality
 */
void test_jpeg_display(void)
{
    ESP_LOGI(TAG, "Testing JPEG display functionality");

    // Initialize GFX system
    gfx_core_config_t cfg = {
        .h_res = 320,
        .v_res = 240,
        .fps = 30,
        .flush_cb = NULL,  // You would set your display flush callback here
        .update_cb = NULL,
        .flags = {
            .swap = false,
            .double_buffer = true,
            .buff_dma = false,
            .buff_spiram = false,
        },
        .buffers = {
            .buf1 = NULL,
            .buf2 = NULL,
            .buf_pixels = 0,
        },
        .task = GFX_EMOTE_INIT_CONFIG(),
    };

    gfx_handle_t handle = gfx_emote_init(&cfg);
    if (handle == NULL) {
        ESP_LOGE(TAG, "Failed to initialize GFX system");
        return;
    }

    // Method 1: Display JPEG using descriptor structure
    ESP_LOGI(TAG, "Method 1: Using JPEG descriptor");
    gfx_obj_t *img1 = display_jpeg_from_memory(handle, example_jpeg_data, example_jpeg_size, 10, 10);
    if (img1) {
        ESP_LOGI(TAG, "JPEG image created successfully");
    } else {
        ESP_LOGE(TAG, "Failed to create JPEG image");
    }

    // Method 2: Direct raw JPEG data (for comparison)
    ESP_LOGI(TAG, "Method 2: Using raw JPEG data");
    gfx_obj_t *img2 = gfx_img_create(handle);
    if (img2) {
        // For raw JPEG data, we need to pass size information somehow
        // This is a limitation of the current implementation
        gfx_img_set_src(img2, (void *)example_jpeg_data);
        gfx_obj_set_pos(img2, 200, 10);
        ESP_LOGI(TAG, "Raw JPEG image created");
    }

    // In a real application, you would now render and display these images
    ESP_LOGI(TAG, "JPEG test completed");

    // Cleanup
    if (img1) {
        cleanup_jpeg_image(img1);
    }
    if (img2) {
        gfx_obj_del(img2);
    }

    gfx_emote_deinit(handle);
}

/**
 * @brief Main application entry point (for testing)
 */
void app_main(void)
{
    ESP_LOGI(TAG, "ESP Emote GFX JPEG Test");
    test_jpeg_display();
}