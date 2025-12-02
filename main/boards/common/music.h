#ifndef MUSIC_H
#define MUSIC_H

#include <string>
#include <functional>

class Music {
public:
    virtual ~Music() = default;  // 添加虚析构函数

    // 旧的搜索功能(已废弃,提供默认空实现以保持兼容性)
    virtual bool Download(const std::string& song_name, const std::string& artist_name = "") {
        return false;  // 默认不支持
    }
    virtual std::string GetDownloadResult() {
        return "";  // 默认返回空字符串
    }

    // 新增流式播放相关方法
    virtual bool StartStreaming(const std::string& music_url) = 0;
    virtual bool StopStreaming() = 0;  // 停止流式播放
    virtual bool StartStreamingFromPosition(const std::string& music_url, int64_t position_ms) {
        // 默认实现：忽略位置参数，调用普通的StartStreaming
        return StartStreaming(music_url);
    }
    virtual size_t GetBufferSize() const = 0;
    virtual bool IsDownloading() const = 0;
    virtual bool IsPlaying() const = 0;
    virtual bool IsPaused() const = 0;
    virtual int16_t* GetAudioData() = 0;
    virtual int GetCurrentPositionSeconds() const = 0;  // 获取当前播放位置(秒)
    virtual int GetCurrentPositionMilliseconds() const = 0;

    // MCP工具需要的方法
    virtual bool PlaySong() = 0;
    virtual bool SetVolume(int volume) = 0;
    virtual bool StopSong() = 0;
    virtual bool PauseSong() = 0;
    virtual bool ResumeSong() = 0;

    // 回调设置方法(提供空实现,子类可选择性覆盖)
    virtual void SetSongFinishedCallback(std::function<void()> callback) {
        // 默认空实现
    }
    virtual void SetErrorCallback(std::function<void(const std::string&)> callback) {
        // 默认空实现
    }

    virtual void SetAutomated(bool is_automated) = 0;
};

#endif // MUSIC_H 