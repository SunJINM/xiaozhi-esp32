/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file jpeg_display_example.h
 * @brief Example header showing how to display JPEG images using ESP Emote GFX
 * 
 * This example demonstrates:
 * 1. Loading JPEG data from memory or file
 * 2. Creating an image object to display JPEG
 * 3. Setting position and displaying the JPEG image
 * 
 * Usage:
 * 1. Include your JPEG data as a byte array or load from file
 * 2. Create the image object with gfx_img_create()
 * 3. Set the JPEG data using gfx_img_set_src()
 * 4. Position and display the image
 */

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/* Note: Use gfx_jpeg_dsc_t from gfx_img.h for JPEG descriptors */

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * @brief Example: Display JPEG from memory
 * @param handle Graphics handle
 * @param jpeg_data Pointer to JPEG data
 * @param jpeg_size Size of JPEG data
 * @param x X coordinate to display image
 * @param y Y coordinate to display image
 * @return gfx_obj_t* Image object or NULL on error
 * 
 * @example
 * @code
 * // Include your JPEG data (e.g., converted from jpg to C array)
 * extern const uint8_t my_image_jpg[];
 * extern const uint32_t my_image_jpg_len;
 * 
 * gfx_obj_t *img = display_jpeg_from_memory(handle, my_image_jpg, my_image_jpg_len, 50, 100);
 * @endcode
 */
static inline gfx_obj_t *display_jpeg_from_memory(gfx_handle_t handle, 
                                                  const uint8_t *jpeg_data, 
                                                  uint32_t jpeg_size,
                                                  gfx_coord_t x, 
                                                  gfx_coord_t y)
{
    if (!handle || !jpeg_data || jpeg_size == 0) {
        return NULL;
    }

    // Create image object
    gfx_obj_t *img = gfx_img_create(handle);
    if (!img) {
        return NULL;
    }

    // Create JPEG descriptor
    gfx_jpeg_dsc_t *jpeg_dsc = malloc(sizeof(gfx_jpeg_dsc_t));
    if (!jpeg_dsc) {
        gfx_obj_del(img);
        return NULL;
    }
    
    // Initialize JPEG descriptor
    jpeg_dsc->header.magic = 0xFFD8FFE0;  // JPEG magic
    jpeg_dsc->header.cf = GFX_COLOR_FORMAT_RGB565A8;
    jpeg_dsc->header.w = 0;  // Will be determined during decode
    jpeg_dsc->header.h = 0;  // Will be determined during decode
    jpeg_dsc->header.stride = 0;
    jpeg_dsc->header.reserved = 0;
    jpeg_dsc->data_size = jpeg_size;
    jpeg_dsc->data = jpeg_data;
    jpeg_dsc->reserved = NULL;
    jpeg_dsc->reserved_2 = NULL;

    // Set JPEG descriptor as source
    gfx_img_set_src(img, jpeg_dsc);
    
    // Set position
    gfx_obj_set_pos(img, x, y);
    
    // Store descriptor for cleanup
    gfx_obj_set_user_data(img, jpeg_dsc);

    return img;
}

/**
 * @brief Example: Clean up JPEG image object
 * @param img Image object to clean up
 */
static inline void cleanup_jpeg_image(gfx_obj_t *img)
{
    if (img) {
        void *user_data = gfx_obj_get_user_data(img);
        if (user_data) {
            free(user_data);
        }
        gfx_obj_del(img);
    }
}

#ifdef __cplusplus
}
#endif