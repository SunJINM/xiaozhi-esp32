#ifndef ESP32_MUSIC_H
#define ESP32_MUSIC_H

#include <string>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <functional>

#include "music.h"

// MP3解码器支持
extern "C" {
#include "mp3dec.h"
}

// 音频数据块结构
struct AudioChunk {
    uint8_t* data;
    size_t size;
    
    AudioChunk() : data(nullptr), size(0) {}
    AudioChunk(uint8_t* d, size_t s) : data(d), size(s) {}
};

class Esp32Music : public Music {
public:
    // 显示模式控制 - 移动到public区域
    enum DisplayMode {
        DISPLAY_MODE_SPECTRUM = 0,  // 默认显示频谱
        DISPLAY_MODE_LYRICS = 1     // 显示歌词
    };

private:
    std::string last_downloaded_data_;
    std::string current_music_url_;
    std::string current_song_name_;
    bool song_name_displayed_;

    std::atomic<DisplayMode> display_mode_;
    std::atomic<bool> is_playing_;
    std::atomic<bool> is_downloading_;
    std::atomic<bool> is_paused_;
    std::atomic<bool> is_waiting_;
    std::atomic<bool> is_automated_;
    std::thread play_thread_;
    std::thread download_thread_;
    int64_t current_play_time_ms_;  // 当前播放时间(毫秒)
    int64_t last_frame_time_ms_;    // 上一帧的时间戳
    int total_frames_decoded_;      // 已解码的帧数

    // 音频缓冲区
    std::queue<AudioChunk> audio_buffer_;
    std::mutex buffer_mutex_;
    std::condition_variable buffer_cv_;
    size_t buffer_size_;
    static constexpr size_t MAX_BUFFER_SIZE = 256 * 1024;  // 256KB缓冲区（降低以减少brownout风险）
    static constexpr size_t MIN_BUFFER_SIZE = 32 * 1024;   // 32KB最小播放缓冲（降低以减少brownout风险）
    
    // MP3解码器相关
    HMP3Decoder mp3_decoder_;
    MP3FrameInfo mp3_frame_info_;
    bool mp3_decoder_initialized_;
    
    // 私有方法
    void DownloadAudioStream(const std::string& music_url);
    void PlayAudioStream();
    void ClearAudioBuffer();
    bool InitializeMp3Decoder();
    void CleanupMp3Decoder();
    void ResetSampleRate();  // 重置采样率到原始值

    // ID3标签处理
    size_t SkipId3Tag(uint8_t* data, size_t size);

    int16_t* final_pcm_data_fft = nullptr;

    // 回调函数
    std::function<void()> on_song_finished_;
    std::function<void(const std::string&)> on_error_;

public:
    Esp32Music();
    ~Esp32Music();
    
    // 新增方法
    virtual bool StartStreaming(const std::string& music_url) override;
    virtual bool StopStreaming() override;  // 停止流式播放
    virtual size_t GetBufferSize() const override { return buffer_size_; }
    virtual bool IsDownloading() const override { return is_downloading_; }
    virtual bool IsPlaying() const override { return is_playing_; }
    virtual bool IsPaused() const override { return is_paused_; }
    virtual int16_t* GetAudioData() override { return final_pcm_data_fft; }
    virtual int GetCurrentPositionSeconds() const override { return static_cast<int>(current_play_time_ms_ / 1000); }

    // 显示模式控制方法
    void SetDisplayMode(DisplayMode mode);
    DisplayMode GetDisplayMode() const { return display_mode_.load(); }

    // 回调设置
    void SetSongFinishedCallback(std::function<void()> callback) { on_song_finished_ = callback; }
    void SetErrorCallback(std::function<void(const std::string&)> callback) { on_error_ = callback; }

    void SetAutomated(bool is_automated);

    virtual bool PlaySong() override;
    virtual bool SetVolume(int volume) override;
    virtual bool StopSong() override;
    virtual bool PauseSong() override;
    virtual bool ResumeSong() override;
};

#endif // ESP32_MUSIC_H
