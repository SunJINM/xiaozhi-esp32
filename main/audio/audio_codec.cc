#include "audio_codec.h"
#include "board.h"
#include "settings.h"

#include <esp_log.h>
#include <cstring>
#include <driver/i2s_common.h>

#define TAG "AudioCodec"

AudioCodec::AudioCodec() {
}

AudioCodec::~AudioCodec() {
}

void AudioCodec::OutputData(std::vector<int16_t>& data) {
    Write(data.data(), data.size());
}

bool AudioCodec::InputData(std::vector<int16_t>& data) {
    int samples = Read(data.data(), data.size());
    if (samples > 0) {
        return true;
    }
    return false;
}

void AudioCodec::Start() {
    Settings settings("audio", false);
    output_volume_ = settings.GetInt("output_volume", output_volume_);
    if (output_volume_ <= 0) {
        ESP_LOGW(TAG, "Output volume value (%d) is too small, setting to default (10)", output_volume_);
        output_volume_ = 10;
    }
    output_volume_ = 50;


    if (tx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    }

    if (rx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    }

    EnableInput(true);
    EnableOutput(true);
    ESP_LOGI(TAG, "Audio codec started");
}

void AudioCodec::SetOutputVolume(int volume) {
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);
    
    Settings settings("audio", true);
    settings.SetInt("output_volume", output_volume_);
}

void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}  

void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}

void AudioCodec::Flush() {
    if (tx_handle_ != nullptr) {
        // 播放中的通道不能用preload_data,改用静音数据快速清空
        const size_t silence_samples = 480; // 10ms @ 48kHz
        int16_t silence_buffer[silence_samples] = {0};
        size_t bytes_written = 0;

        // 写入3次静音数据(约30ms),快速覆盖DMA缓冲区
        for (int i = 0; i < 3; i++) {
            esp_err_t ret = i2s_channel_write(tx_handle_, silence_buffer,
                                              sizeof(silence_buffer), &bytes_written, 10);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Flush: failed to write silence data (attempt %d)", i + 1);
                break;
            }
        }
        ESP_LOGI(TAG, "Flushed audio output buffer with silence");
    }
}
