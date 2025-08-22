/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "core/gfx_core.h"
#include "core/gfx_obj.h"
#include "widget/gfx_img.h"
#include "decoder/gfx_img_decoder.h"
#include "decoder/gfx_aaf_dec.h"
#include "decoder/gfx_jpeg_dec.h"

static const char *TAG = "gfx_img_decoder";

/*********************
 *      DEFINES
 *********************/

#define MAX_DECODERS 8

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/

static esp_err_t image_format_info_cb(gfx_image_decoder_t *decoder, gfx_image_decoder_dsc_t *dsc, gfx_image_header_t *header);
static esp_err_t image_format_open_cb(gfx_image_decoder_t *decoder, gfx_image_decoder_dsc_t *dsc);
static void image_format_close_cb(gfx_image_decoder_t *decoder, gfx_image_decoder_dsc_t *dsc);

static esp_err_t jpeg_format_info_cb(gfx_image_decoder_t *decoder, gfx_image_decoder_dsc_t *dsc, gfx_image_header_t *header);
static esp_err_t jpeg_format_open_cb(gfx_image_decoder_t *decoder, gfx_image_decoder_dsc_t *dsc);
static void jpeg_format_close_cb(gfx_image_decoder_t *decoder, gfx_image_decoder_dsc_t *dsc);

static esp_err_t aaf_format_info_cb(gfx_image_decoder_t *decoder, gfx_image_decoder_dsc_t *dsc, gfx_image_header_t *header);
static esp_err_t aaf_format_open_cb(gfx_image_decoder_t *decoder, gfx_image_decoder_dsc_t *dsc);
static void aaf_format_close_cb(gfx_image_decoder_t *decoder, gfx_image_decoder_dsc_t *dsc);

/**********************
 *  STATIC VARIABLES
 **********************/

static gfx_image_decoder_t *registered_decoders[MAX_DECODERS] = {NULL};
static uint8_t decoder_count = 0;

// Built-in decoders
static gfx_image_decoder_t image_decoder = {
    .name = "IMAGE",
    .info_cb = image_format_info_cb,
    .open_cb = image_format_open_cb,
    .close_cb = image_format_close_cb,
};

static gfx_image_decoder_t jpeg_decoder = {
    .name = "JPEG",
    .info_cb = jpeg_format_info_cb,
    .open_cb = jpeg_format_open_cb,
    .close_cb = jpeg_format_close_cb,
};

static gfx_image_decoder_t aaf_decoder = {
    .name = "AAF",
    .info_cb = aaf_format_info_cb,
    .open_cb = aaf_format_open_cb,
    .close_cb = aaf_format_close_cb,
};

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/*=====================
 * Image format detection
 *====================*/

gfx_image_format_t gfx_image_detect_format(const void *src)
{
    if (src == NULL) {
        return GFX_IMAGE_FORMAT_UNKNOWN;
    }

    uint8_t *byte_ptr = (uint8_t *)src;

    // Check for C_ARRAY format
    if (byte_ptr[0] == C_ARRAY_HEADER_MAGIC) {
        return GFX_IMAGE_FORMAT_C_ARRAY;
    }

    ESP_LOGI(TAG, "JPEG: byte_ptr[0]=0x%02X%02X,", byte_ptr[0], byte_ptr[1]);
    // Check for JPEG format (0xFF 0xD8 magic)
    if (byte_ptr[0] == 0xFF && byte_ptr[1] == 0xD8) {
        return GFX_IMAGE_FORMAT_JPEG;
    }

    // Check for JPEG descriptor format (magic = 0xFFD8xxxx)
    uint32_t *word_ptr = (uint32_t *)src;
    if ((word_ptr[0] & 0x0000FFFF) == 0x0000D8FF) { // Little-endian JPEG magic in header
        return GFX_IMAGE_FORMAT_JPEG;
    }

    // Check for AAF format (0x89 "AAF" magic)
    if (byte_ptr[0] == 0x89 && byte_ptr[1] == 'A' && byte_ptr[2] == 'A' && byte_ptr[3] == 'F') {
        return GFX_IMAGE_FORMAT_AAF;
    }

    return GFX_IMAGE_FORMAT_UNKNOWN;
}

/*=====================
 * Image decoder functions
 *====================*/

esp_err_t gfx_image_decoder_register(gfx_image_decoder_t *decoder)
{
    if (decoder == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (decoder_count >= MAX_DECODERS) {
        ESP_LOGE(TAG, "Too many decoders registered");
        return ESP_ERR_NO_MEM;
    }

    registered_decoders[decoder_count] = decoder;
    decoder_count++;

    ESP_LOGD(TAG, "Registered decoder: %s", decoder->name);
    return ESP_OK;
}

esp_err_t gfx_image_decoder_info(gfx_image_decoder_dsc_t *dsc, gfx_image_header_t *header)
{
    if (dsc == NULL || header == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Try each registered decoder
    for (int i = 0; i < decoder_count; i++) {
        gfx_image_decoder_t *decoder = registered_decoders[i];
        if (decoder && decoder->info_cb) {
            ESP_LOGI(TAG, "Decoder %s found format", decoder->name);
            esp_err_t ret = decoder->info_cb(decoder, dsc, header);
            if (ret == ESP_OK) {
                ESP_LOGD(TAG, "Decoder %s found format", decoder->name);
                return ESP_OK;
            }
        }
    }

    ESP_LOGW(TAG, "gfx_image_decoder_info failed: no suitable decoder found");
    return ESP_ERR_INVALID_ARG;
}

esp_err_t gfx_image_decoder_open(gfx_image_decoder_dsc_t *dsc)
{
    if (dsc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Try each registered decoder
    for (int i = 0; i < decoder_count; i++) {
        gfx_image_decoder_t *decoder = registered_decoders[i];
        if (decoder && decoder->open_cb) {
            esp_err_t ret = decoder->open_cb(decoder, dsc);
            if (ret == ESP_OK) {
                ESP_LOGD(TAG, "Decoder %s opened image", decoder->name);
                return ESP_OK;
            }
        }
    }

    ESP_LOGW(TAG, "No decoder could open image");
    return ESP_ERR_INVALID_ARG;
}

void gfx_image_decoder_close(gfx_image_decoder_dsc_t *dsc)
{
    if (dsc == NULL) {
        return;
    }

    // Try each registered decoder
    for (int i = 0; i < decoder_count; i++) {
        gfx_image_decoder_t *decoder = registered_decoders[i];
        if (decoder && decoder->close_cb) {
            decoder->close_cb(decoder, dsc);
        }
    }
}

/*=====================
 * Built-in decoder implementations
 *====================*/

// C_ARRAY format decoder
static esp_err_t image_format_info_cb(gfx_image_decoder_t *decoder, gfx_image_decoder_dsc_t *dsc, gfx_image_header_t *header)
{
    if (dsc->src == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gfx_image_format_t format = gfx_image_detect_format(dsc->src);
    if (format != GFX_IMAGE_FORMAT_C_ARRAY) {
        return ESP_ERR_INVALID_ARG;
    }

    gfx_image_dsc_t *image_desc = (gfx_image_dsc_t *)dsc->src;
    memcpy(header, &image_desc->header, sizeof(gfx_image_header_t));

    return ESP_OK;
}

static esp_err_t image_format_open_cb(gfx_image_decoder_t *decoder, gfx_image_decoder_dsc_t *dsc)
{
    if (dsc->src == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gfx_image_format_t format = gfx_image_detect_format(dsc->src);
    if (format != GFX_IMAGE_FORMAT_C_ARRAY) {
        return ESP_ERR_INVALID_ARG;
    }

    gfx_image_dsc_t *image_desc = (gfx_image_dsc_t *)dsc->src;
    dsc->data = image_desc->data;
    dsc->data_size = image_desc->data_size;

    return ESP_OK;
}

static void image_format_close_cb(gfx_image_decoder_t *decoder, gfx_image_decoder_dsc_t *dsc)
{
    // Nothing to do for C_ARRAY format
}

// JPEG format decoder
static esp_err_t jpeg_format_info_cb(gfx_image_decoder_t *decoder, gfx_image_decoder_dsc_t *dsc, gfx_image_header_t *header)
{
    if (dsc->src == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gfx_image_format_t format = gfx_image_detect_format(dsc->src);
    if (format != GFX_IMAGE_FORMAT_JPEG) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *src_data = (const uint8_t *)dsc->src;
    
    // Check if this is a gfx_jpeg_dsc_t structure or raw JPEG data
    if ((src_data[0] == 0xFF && src_data[1] == 0xD8)) {
        // Raw JPEG data - we need size info from somewhere
        // This is a limitation for raw data without descriptor
        ESP_LOGW(TAG, "Raw JPEG data detected, dimensions will be determined during decode");
        header->magic = 0xFFD8FFE0;
        header->cf = GFX_COLOR_FORMAT_RGB565A8;
        header->w = 0; // Unknown until decode
        header->h = 0; // Unknown until decode
        header->stride = 0;
        header->reserved = 0;
    } else {
        // Assume this is a gfx_jpeg_dsc_t structure
        gfx_jpeg_dsc_t *jpeg_dsc = (gfx_jpeg_dsc_t *)dsc->src;
        memcpy(header, &jpeg_dsc->header, sizeof(gfx_image_header_t));
    }

    return ESP_OK;
}

static esp_err_t jpeg_format_open_cb(gfx_image_decoder_t *decoder, gfx_image_decoder_dsc_t *dsc)
{
    if (dsc->src == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gfx_image_format_t format = gfx_image_detect_format(dsc->src);
    if (format != GFX_IMAGE_FORMAT_JPEG) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t *jpeg_src;
    uint32_t jpeg_size;
    
    // Determine source and size based on data format
    if (((uint8_t *)dsc->src)[0] == 0xFF && ((uint8_t *)dsc->src)[1] == 0xD8) {
        // Raw JPEG data
        jpeg_src = (const uint8_t *)dsc->src;
        jpeg_size = dsc->data_size; // Must be provided by caller
        if (jpeg_size == 0) {
            ESP_LOGE(TAG, "JPEG size must be provided for raw data");
            return ESP_ERR_INVALID_SIZE;
        }
    } else {
        // JPEG descriptor structure
        gfx_jpeg_dsc_t *jpeg_dsc = (gfx_jpeg_dsc_t *)dsc->src;
        jpeg_src = jpeg_dsc->data;
        jpeg_size = jpeg_dsc->data_size;
        if (jpeg_src == NULL || jpeg_size == 0) {
            ESP_LOGE(TAG, "Invalid JPEG descriptor");
            return ESP_ERR_INVALID_ARG;
        }
    }

    // Decode JPEG to RGB565 format
    uint32_t width, height;
    
    // First pass: get dimensions
    esp_err_t ret = gfx_jpeg_decode(jpeg_src, jpeg_size, NULL, 0, &width, &height, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get JPEG dimensions");
        return ret;
    }

    // Allocate buffer for decoded RGB565 data + alpha channel
    size_t rgb_size = width * height * 2; // RGB565
    size_t alpha_size = width * height;   // Alpha channel
    size_t total_size = rgb_size + alpha_size;
    
    uint8_t *decoded_buffer = malloc(total_size);
    if (decoded_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate decode buffer");
        return ESP_ERR_NO_MEM;
    }

    // Decode JPEG to RGB565 format
    ret = gfx_jpeg_decode(jpeg_src, jpeg_size, decoded_buffer, rgb_size, &width, &height, false);
    if (ret != ESP_OK) {
        free(decoded_buffer);
        ESP_LOGE(TAG, "Failed to decode JPEG");
        return ret;
    }

    // Fill alpha channel (JPEG doesn't have transparency, so set to opaque)
    uint8_t *alpha_channel = decoded_buffer + rgb_size;
    memset(alpha_channel, 255, alpha_size); // Fully opaque

    // Update header with actual dimensions
    dsc->header.w = width;
    dsc->header.h = height;
    dsc->header.stride = width * 2;
    
    // Set decoded data
    dsc->data = decoded_buffer;
    dsc->data_size = total_size;
    dsc->user_data = decoded_buffer; // Store for cleanup

    return ESP_OK;
}

static void jpeg_format_close_cb(gfx_image_decoder_t *decoder, gfx_image_decoder_dsc_t *dsc)
{
    if (dsc->user_data != NULL) {
        free(dsc->user_data);
        dsc->user_data = NULL;
        dsc->data = NULL;
        dsc->data_size = 0;
    }
}

// AAF format decoder
static esp_err_t aaf_format_info_cb(gfx_image_decoder_t *decoder, gfx_image_decoder_dsc_t *dsc, gfx_image_header_t *header)
{
    if (dsc->src == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gfx_image_format_t format = gfx_image_detect_format(dsc->src);
    if (format != GFX_IMAGE_FORMAT_AAF) {
        return ESP_ERR_INVALID_ARG;
    }

    // // Parse AAF header to get basic info
    // const uint8_t *aaf_data = (const uint8_t *)dsc->src;

    // // Skip magic (4 bytes)
    // uint32_t total_files = *(uint32_t *)(aaf_data + 4);
    // uint32_t checksum = *(uint32_t *)(aaf_data + 8);
    // uint32_t data_length = *(uint32_t *)(aaf_data + 12);

    // For AAF, we can't easily determine width/height without parsing the first frame
    // So we'll set default values and let the animation system handle the details
    // header->magic = 0x89414146; // 0x89 "AAF"
    // header->cf = GFX_COLOR_FORMAT_RGB565; // Default to RGB565
    // header->w = 0; // Will be determined when frames are loaded
    // header->h = 0; // Will be determined when frames are loaded
    // header->stride = 0; // Will be calculated based on actual dimensions
    // header->reserved = total_files; // Store frame count in reserved field

    return ESP_OK;
}

static esp_err_t aaf_format_open_cb(gfx_image_decoder_t *decoder, gfx_image_decoder_dsc_t *dsc)
{
    if (dsc->src == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gfx_image_format_t format = gfx_image_detect_format(dsc->src);
    if (format != GFX_IMAGE_FORMAT_AAF) {
        return ESP_ERR_INVALID_ARG;
    }

    // For AAF format, we return the entire file data
    // The animation system will handle frame extraction
    dsc->data = (const uint8_t *)dsc->src;
    dsc->data_size = 0; // Size will be determined by the animation system

    return ESP_OK;
}

static void aaf_format_close_cb(gfx_image_decoder_t *decoder, gfx_image_decoder_dsc_t *dsc)
{
    // Nothing to do for AAF format
}

/*=====================
 * Initialization
 *====================*/

esp_err_t gfx_image_decoder_init(void)
{
    // Register built-in decoders
    esp_err_t ret = gfx_image_decoder_register(&image_decoder);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gfx_image_decoder_register(&jpeg_decoder);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gfx_image_decoder_register(&aaf_decoder);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "Image decoder system initialized with %d decoders", decoder_count);
    return ESP_OK;
}

esp_err_t gfx_image_decoder_deinit(void)
{
    // Clear all registered decoders
    for (int i = 0; i < decoder_count; i++) {
        registered_decoders[i] = NULL;
    }

    // Reset decoder count
    decoder_count = 0;

    ESP_LOGI(TAG, "Image decoder system deinitialized");
    return ESP_OK;
}