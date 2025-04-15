#include "iot/thing.h"
#include "application.h"
#include "protocol.h"
#include "app_camera.h"
#include <driver/i2c.h>

#include <esp_log.h>

#define TAG "Camera"

namespace iot {

class Camera : public Thing {
private:
    QueueHandle_t photo_queue_;  // 添加这行
    AppCamera* camera_;
public:
    Camera() : Thing("Camera", "Camera device") {

        // 创建照片队列
        photo_queue_ = xQueueCreate(2, sizeof(camera_fb_t*));

        // 初始化摄像头
        camera_ = new AppCamera(PIXFORMAT_RGB565, FRAMESIZE_240X240, 2, photo_queue_);
        if (camera_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create camera");
            return;
        }
        camera_->run();

        // 添加拍照方法
        methods_.AddMethod("take_photo", "Take a photo", ParameterList(), [this](const ParameterList&) {
            TakePhoto();
        });
    }

    void TakePhoto() {
        ESP_LOGI(TAG, "Taking photo...");
        camera_fb_t* fb = nullptr;
        if (xQueueReceive(photo_queue_, &fb, pdMS_TO_TICKS(5000)) == pdTRUE) {
            if (fb) {
                std::vector<uint8_t> photo_data(fb->buf, fb->buf + fb->len);
                SendPhoto(photo_data);
                esp_camera_fb_return(fb);
            }
        }
    }
    
    void SendPhoto(const std::vector<uint8_t>& photo_data) {
        ESP_LOGI(TAG, "Sending photo...");
    }

};

} // namespace iot 
