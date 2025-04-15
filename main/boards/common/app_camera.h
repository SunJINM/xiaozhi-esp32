#pragma once

#include <list>
#include "esp_camera.h"
#include "board_config.hpp"
#include "__base__.hpp"

#define CAMERA_MODULE_NAME "ESP-S3-EYE"

class AppCamera : public Frame
{
public:
    AppCamera(const pixformat_t pixel_fromat,
              const framesize_t frame_size,
              const uint8_t fb_count,
              QueueHandle_t queue_o = nullptr);

    void run();
    
    // 获取当前图片帧数据
    camera_fb_t* get_current_frame();
};
