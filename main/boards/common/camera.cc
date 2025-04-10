#include "camera.h"

#include "esp_log.h"
#include "esp_system.h"

const static char TAG[] = "Camera";

AppCamera::AppCamera(const pixformat_t pixel_fromat,
                     const framesize_t frame_size,
                     const uint8_t fb_count,
                     QueueHandle_t queue_o) : Frame(nullptr, queue_o, nullptr)
{
    ESP_LOGI(TAG, "Camera module is %s", CAMERA_MODULE_NAME);
    const camera_config_t camera_config = BSP_CAMERA_DEFAULT_CONFIG;
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera Init Failed");
        return;
    }
    sensor_t *s = esp_camera_sensor_get();
    s->set_vflip(s, BSP_CAMERA_VFLIP);
    s->set_hmirror(s, BSP_CAMERA_HMIRROR);
}

static void task(AppCamera *self)
{
    ESP_LOGD(TAG, "Start");
    while (true)
    {
        if (self->queue_o == nullptr)
            break;

        camera_fb_t *frame = esp_camera_fb_get();
        if (frame)
            xQueueSend(self->queue_o, &frame, portMAX_DELAY);
    }
    ESP_LOGD(TAG, "Stop");
    vTaskDelete(NULL);
}

void AppCamera::run()
{
    xTaskCreatePinnedToCore((TaskFunction_t)task, TAG, 2 * 1024, this, 5, NULL, 0);
}
