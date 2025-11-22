#include <esp_log.h>
#include <esp_err.h>
#include <string>
#include <cstring>

#include "display.h"
#include "settings.h"
#include <lvgl.h>

#if LV_USE_QRCODE
#include "libs/qrcode/lv_qrcode.h"
#endif

#define TAG "Display"

Display::Display() {
    // Create a power management lock
    auto ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "display_update", &pm_lock_);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "Power management not supported");
    } else {
        ESP_ERROR_CHECK(ret);
    }
}

Display::~Display() {
    if (pm_lock_ != nullptr) {
        esp_pm_lock_delete(pm_lock_);
    }
}

void Display::SetStatus(const char* status) {
    ESP_LOGI(TAG, "SetStatus: %s", status);
}

void Display::ShowNotification(const char* notification, int duration_ms) {
    ESP_LOGI(TAG, "ShowNotification: %s, duration: %d", notification, duration_ms);
}

void Display::UpdateStatusBar(bool update_all) {
    ESP_LOGI(TAG, "UpdateStatusBar: update_all=%d", update_all);
}

void Display::SetEmotion(const char* emotion) {
    ESP_LOGI(TAG, "SetEmotion: %s", emotion);
}

void Display::SetIcon(const char* icon) {
    ESP_LOGI(TAG, "SetIcon: %s", icon);
}

void Display::SetPreviewImage(const void* image) {
    ESP_LOGI(TAG, "SetPreviewImage");
}

void Display::SetChatMessage(const char* role, const char* content) {
    ESP_LOGI(TAG, "SetChatMessage: role=%s, content=%s", role ? role : "null", content ? content : "null");
}

void Display::SetTheme(const std::string& theme_name) {
    current_theme_name_ = theme_name;
    Settings settings("display", true);
    settings.SetString("theme", theme_name);
}

void Display::SetPowerSaveMode(bool on) {
    if (on) {
        SetChatMessage("system", "");
        SetEmotion("sleepy");
    } else {
        SetChatMessage("system", "");
        SetEmotion("neutral");
    }
}

// 音乐播放相关（用日志模拟UI行为）
// 显示当前播放的歌曲信息
void Display::SetMusicInfo(const char* info)
{
    ESP_LOGW(TAG, "MusicInfo: %s", info ? info : "");
}

// 启动频谱显示（此处仅打印日志）
void Display::start()
{
    ESP_LOGW(TAG, "Spectrum start");
}

// 停止频谱显示（此处仅打印日志）
void Display::stopFft()
{
    ESP_LOGW(TAG, "Spectrum stop");
}

// 显示 QR 码
void Display::ShowQrCode(const char* data) {
    if (!data) {
        ESP_LOGE(TAG, "QR code data is null");
        return;
    }

#if LV_USE_QRCODE
    ESP_LOGI(TAG, "ShowQrCode: %s", data);

    // 如果已经存在 QR 码对象,先删除
    if (qr_code_obj_) {
        lv_obj_delete(qr_code_obj_);
        qr_code_obj_ = nullptr;
    }

    // 创建 QR 码对象
    qr_code_obj_ = lv_qrcode_create(lv_screen_active());
    if (!qr_code_obj_) {
        ESP_LOGE(TAG, "Failed to create QR code object");
        return;
    }

    // 根据屏幕尺寸设置 QR 码大小
    // 对于圆形屏幕,需要考虑内切圆的有效显示区域
    int min_dimension = (width_ < height_ ? width_ : height_);
    int qr_size = min_dimension / 2;  // 使用屏幕最小边的一半,适配圆形屏幕
    if (qr_size < 100) qr_size = 100;  // 最小 100 像素
    if (qr_size > 200) qr_size = 200;  // 最大 200 像素,确保在圆形区域内

    lv_qrcode_set_size(qr_code_obj_, qr_size);
    lv_qrcode_set_dark_color(qr_code_obj_, lv_color_black());
    lv_qrcode_set_light_color(qr_code_obj_, lv_color_white());

    // 更新 QR 码数据
    lv_result_t result = lv_qrcode_update(qr_code_obj_, data, strlen(data));
    if (result != LV_RESULT_OK) {
        ESP_LOGE(TAG, "Failed to update QR code data");
        lv_obj_delete(qr_code_obj_);
        qr_code_obj_ = nullptr;
        return;
    }

    // 居中显示
    lv_obj_center(qr_code_obj_);

    // 添加白色背景和边框,确保二维码清晰可见
    lv_obj_set_style_bg_color(qr_code_obj_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(qr_code_obj_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(qr_code_obj_, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr_code_obj_, 10, 0);  // 增加边框宽度
    lv_obj_set_style_pad_all(qr_code_obj_, 5, 0);  // 添加内边距

    ESP_LOGI(TAG, "QR code displayed successfully (size: %d, screen: %dx%d)", qr_size, width_, height_);
#else
    ESP_LOGW(TAG, "QR code support not enabled (LV_USE_QRCODE=0)");
    ESP_LOGI(TAG, "QR code data: %s", data);
#endif
}

// 清除 QR 码显示
void Display::ClearQrCode() {
    ESP_LOGI(TAG, "ClearQrCode");

#if LV_USE_QRCODE
    if (qr_code_obj_) {
        // 检查对象是否仍然有效
        if (lv_obj_is_valid(qr_code_obj_)) {
            lv_obj_delete(qr_code_obj_);
            ESP_LOGI(TAG, "QR code cleared");
        } else {
            ESP_LOGW(TAG, "QR code object already invalid");
        }
        qr_code_obj_ = nullptr;
    }
#endif
}
