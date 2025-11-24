# 音乐播放优化实现指南

**文档版本**: 1.0
**创建日期**: 2025-11-24
**用途**: 指导完成音乐播放优化的剩余集成工作

---

## 已完成工作 ✅

### Phase 1 & 2: MusicPlaylistManager 类 ✅
- **文件**: `main/boards/common/music_playlist_manager.h`
- **文件**: `main/boards/common/music_playlist_manager.cc`
- **功能**: 完整的播放列表管理和播放模式逻辑
- **状态**: 已实现并可直接使用

### Phase 3: Esp32Music 头文件更新 ✅
- **文件**: `main/boards/common/esp32_music.h`
- **修改**:
  - 移除歌词相关字段(`current_lyric_url_`, `lyrics_`, `lyric_thread_`等)
  - 注释掉`Download()`和`GetDownloadResult()`方法
  - 添加回调机制:`on_song_finished_`, `on_error_`
  - 添加回调设置方法:`SetSongFinishedCallback()`, `SetErrorCallback()`

---

## 待完成工作 🚧

### Phase 3 (续): Esp32Music 实现文件修改

**文件**: `main/boards/common/esp32_music.cc` (1549行,需要手工修改)

#### 需要修改的地方:

#### 1. 构造函数初始化
```cpp
Esp32Music::Esp32Music()
    : song_name_displayed_(false)
    , display_mode_(DISPLAY_MODE_SPECTRUM)
    , is_playing_(false)
    , is_downloading_(false)
    , is_paused_(false)
    , is_waiting_(false)
    , current_play_time_ms_(0)
    , last_frame_time_ms_(0)
    , total_frames_decoded_(0)
    , buffer_size_(0)
    , mp3_decoder_(nullptr)
    , mp3_decoder_initialized_(false)
    // 移除歌词相关初始化
    // , current_lyric_index_(0)
    // , is_lyric_running_(false)
    , on_song_finished_(nullptr)  // 新增
    , on_error_(nullptr) {        // 新增
}
```

#### 2. 析构函数
```cpp
Esp32Music::~Esp32Music() {
    StopStreaming();

    // 移除歌词线程停止逻辑
    // if (is_lyric_running_) {
    //     is_lyric_running_ = false;
    //     if (lyric_thread_.joinable()) {
    //         lyric_thread_.join();
    //     }
    // }

    if (final_pcm_data_fft) {
        heap_caps_free(final_pcm_data_fft);
        final_pcm_data_fft = nullptr;
    }

    CleanupMp3Decoder();
}
```

#### 3. 移除/注释歌词相关方法实现
```cpp
// 以下方法需要删除或注释掉:
// bool Esp32Music::DownloadLyrics(const std::string& lyric_url)
// bool Esp32Music::ParseLyrics(const std::string& lyric_content)
// void Esp32Music::LyricDisplayThread()
// void Esp32Music::UpdateLyricDisplay(int64_t current_time_ms)

// 以下方法需要删除或注释掉:
// bool Esp32Music::Download(const std::string& song_name, const std::string& artist_name)
// std::string Esp32Music::GetDownloadResult()
```

#### 4. PlayAudioStream() - 添加播放完成检测

在`PlayAudioStream()`方法的播放循环结束时,添加播放完成回调:

```cpp
void Esp32Music::PlayAudioStream() {
    // ... 现有的播放逻辑 ...

    // 在播放循环结束时(正常结束或停止):
    ESP_LOGI(TAG, "Play thread finished");

    // 检查是否正常播完(不是用户停止)
    if (!is_playing_ && !is_paused_ && on_song_finished_) {
        ESP_LOGI(TAG, "Song finished, calling callback");
        on_song_finished_();  // 调用播放完成回调
    }

    // ... 清理工作 ...
}
```

**关键点**:
- 只在**正常播完**时调用`on_song_finished_`
- 如果是用户手动停止(`StopStreaming()`)不调用回调
- 回调应该在播放线程结束前调用

#### 5. StopStreaming() - 不调用回调

```cpp
bool Esp32Music::StopStreaming() {
    if (!is_playing_ && !is_paused_) {
        return false;
    }

    ESP_LOGI(TAG, "Stopping streaming...");

    is_playing_ = false;
    is_downloading_ = false;
    is_paused_ = false;

    // 移除歌词线程停止
    // if (is_lyric_running_) {
    //     is_lyric_running_ = false;
    // }

    // 等待线程结束
    if (download_thread_.joinable()) {
        download_thread_.join();
    }
    if (play_thread_.joinable()) {
        play_thread_.join();
    }

    // 移除歌词线程等待
    // if (lyric_thread_.joinable()) {
    //     lyric_thread_.join();
    // }

    ClearAudioBuffer();
    CleanupMp3Decoder();

    // 注意:手动停止时不调用on_song_finished_回调

    ESP_LOGI(TAG, "Streaming stopped");
    return true;
}
```

---

### Phase 4: Application 消息处理

**文件**: `main/application.h`

#### 添加到 Application 类私有成员:

```cpp
private:
    // ... 现有成员 ...

    // 音乐播放管理
    std::unique_ptr<MusicPlaylistManager> music_playlist_manager_;
    esp_timer_handle_t music_status_timer_;

    // 音乐控制方法
    void HandleMusicCommand(const cJSON* data);
    void HandleMusicSetPlaylist(const cJSON* data);
    void HandleMusicControl(const std::string& action);
    void HandleMusicSetMode(const cJSON* data);

    // 播放事件回调
    void OnMusicSongFinished();

    // 状态上报
    void SendMusicStatus(bool force = false);
    void StartMusicStatusTimer();
    void StopMusicStatusTimer();
    static void MusicStatusTimerCallback(void* arg);
```

**文件**: `main/application.cc`

#### 1. 包含头文件

```cpp
#include "boards/common/music_playlist_manager.h"
#include "board.h"
#include <cJSON.h>
```

#### 2. 构造函数初始化

```cpp
Application::Application()
    : // ... 现有初始化 ...
    , music_playlist_manager_(std::make_unique<MusicPlaylistManager>())
    , music_status_timer_(nullptr) {
}
```

#### 3. Start() 方法 - 设置回调

```cpp
void Application::Start() {
    // ... 现有代码 ...

    // 设置音乐播放完成回调
    auto music = Board::GetInstance().GetMusic();
    if (music) {
        music->SetSongFinishedCallback([this]() {
            Schedule([this]() {
                OnMusicSongFinished();
            });
        });

        music->SetErrorCallback([this](const std::string& error) {
            Schedule([this, error]() {
                ESP_LOGE("Application", "Music error: %s", error.c_str());
                // 可以选择上报错误状态
                SendMusicStatus(true);
            });
        });
    }

    // ... 现有代码 ...
}
```

#### 4. 协议消息处理 - 添加 music 类型处理

在`OnIncomingJson()`回调中添加(位置:找到其他`type`判断的地方):

```cpp
void Application::OnProtocolMessage(const cJSON* root) {
    auto type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        return;
    }

    // ... 现有 type 判断 ...

    if (strcmp(type->valuestring, "music") == 0) {
        auto data = cJSON_GetObjectItem(root, "data");
        if (data) {
            HandleMusicCommand(data);
        }
        return;
    }

    // ... 现有代码 ...
}
```

#### 5. 实现 HandleMusicCommand

```cpp
void Application::HandleMusicCommand(const cJSON* data) {
    auto command = cJSON_GetObjectItem(data, "command");
    if (!cJSON_IsString(command)) {
        ESP_LOGW("Application", "Music command missing or invalid");
        return;
    }

    std::string cmd = command->valuestring;
    ESP_LOGI("Application", "Music command: %s", cmd.c_str());

    if (cmd == "set_playlist") {
        HandleMusicSetPlaylist(data);
    } else if (cmd == "control") {
        auto action = cJSON_GetObjectItem(data, "action");
        if (cJSON_IsString(action)) {
            HandleMusicControl(action->valuestring);
        }
    } else if (cmd == "set_mode") {
        HandleMusicSetMode(data);
    } else {
        ESP_LOGW("Application", "Unknown music command: %s", cmd.c_str());
    }
}
```

#### 6. 实现 HandleMusicSetPlaylist

参考设计文档中的[示例1: 解析播放列表](music-playback-optimization-design.md#示例1-解析播放列表)

完整代码见设计文档 第989-1063行。

#### 7. 实现 HandleMusicControl

参考设计文档中的[示例2: 播放控制](music-playback-optimization-design.md#示例2-播放控制)

完整代码见设计文档 第1067-1106行。

#### 8. 实现 HandleMusicSetMode

```cpp
void Application::HandleMusicSetMode(const cJSON* data) {
    auto play_mode = cJSON_GetObjectItem(data, "play_mode");
    if (cJSON_IsNumber(play_mode)) {
        music_playlist_manager_->SetPlayMode((PlayMode)play_mode->valueint);
        ESP_LOGI("Application", "Play mode set to %d", play_mode->valueint);
    }

    auto play_type = cJSON_GetObjectItem(data, "play_type");
    if (cJSON_IsNumber(play_type)) {
        music_playlist_manager_->SetPlayType((PlayType)play_type->valueint);
        ESP_LOGI("Application", "Play type set to %d", play_type->valueint);
    }

    // 上报状态
    SendMusicStatus(true);
}
```

#### 9. 实现 OnMusicSongFinished

```cpp
void Application::OnMusicSongFinished() {
    ESP_LOGI("Application", "Song finished, calling playlist manager");
    music_playlist_manager_->OnSongFinished();

    // OnSongFinished内部会:
    // 1. 调用GetNextItem()获取下一曲
    // 2. 如果有下一曲,调用music->StartStreaming()
    // 3. 如果没有下一曲,播放结束
    // 4. 调用SendMusicStatus(true)上报状态

    // 注意:需要在MusicPlaylistManager::OnSongFinished()中添加:
    // Application::GetInstance().SendMusicStatus(true);
}
```

**重要**: 需要修改`music_playlist_manager.cc`中的`OnSongFinished()`方法,添加状态上报调用。

---

### Phase 5: 状态上报实现

#### 1. 实现 SendMusicStatus

```cpp
void Application::SendMusicStatus(bool force) {
    auto music = Board::GetInstance().GetMusic();
    if (!music) {
        return;
    }

    const MusicItem* current = music_playlist_manager_->GetCurrentItem();
    if (!current && !force) {
        return;  // 没有当前播放项且非强制上报,跳过
    }

    // 构建状态JSON
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "music_status");

    // TODO: 添加设备MAC地址
    // cJSON_AddStringToObject(root, "mac", GetDeviceMac().c_str());

    // 播放状态
    int play_status = 0;  // 0-停止, 1-播放中, 2-暂停
    if (music->IsPlaying()) {
        play_status = 1;
    } else if (music->IsPaused()) {
        play_status = 2;
    }
    cJSON_AddNumberToObject(root, "playStatus", play_status);

    // 歌单信息
    cJSON_AddNumberToObject(root, "playlistId", music_playlist_manager_->GetPlaylistId());
    cJSON_AddNumberToObject(root, "resourceType", music_playlist_manager_->GetResourceType());
    cJSON_AddNumberToObject(root, "playMode", music_playlist_manager_->GetPlayMode());
    cJSON_AddNumberToObject(root, "playType", music_playlist_manager_->GetPlayType());

    // 当前播放项信息
    if (current) {
        cJSON_AddNumberToObject(root, "itemId", current->item_id);
        cJSON_AddStringToObject(root, "resourceName", current->resource_name.c_str());
        cJSON_AddNumberToObject(root, "duration", current->duration);
    } else {
        cJSON_AddNumberToObject(root, "itemId", 0);
        cJSON_AddStringToObject(root, "resourceName", "");
        cJSON_AddNumberToObject(root, "duration", 0);
    }

    // 当前播放位置(秒) - TODO: 从Esp32Music获取实际播放时间
    cJSON_AddNumberToObject(root, "currentPosition", 0);

    // 更新时间戳
    cJSON_AddNumberToObject(root, "updateTime", time(nullptr));

    // 发送
    char* json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        SendMcpMessage(json_str);
        cJSON_free(json_str);
    }
    cJSON_Delete(root);

    ESP_LOGI("Application", "Music status sent (playStatus=%d, itemId=%d)",
             play_status, current ? current->item_id : 0);
}
```

#### 2. 定时器回调

```cpp
void Application::MusicStatusTimerCallback(void* arg) {
    Application* app = static_cast<Application*>(arg);
    app->Schedule([app]() {
        app->SendMusicStatus(false);  // 定时器触发,非强制
    });
}
```

#### 3. 启动定时器

```cpp
void Application::StartMusicStatusTimer() {
    if (music_status_timer_) {
        return;  // 已经启动
    }

    const esp_timer_create_args_t timer_args = {
        .callback = &MusicStatusTimerCallback,
        .arg = this,
        .name = "music_status"
    };

    esp_err_t err = esp_timer_create(&timer_args, &music_status_timer_);
    if (err != ESP_OK) {
        ESP_LOGE("Application", "Failed to create music status timer: %s",
                 esp_err_to_name(err));
        return;
    }

    // 每3秒触发一次
    err = esp_timer_start_periodic(music_status_timer_, 3 * 1000 * 1000);  // 3秒(微秒)
    if (err != ESP_OK) {
        ESP_LOGE("Application", "Failed to start music status timer: %s",
                 esp_err_to_name(err));
        esp_timer_delete(music_status_timer_);
        music_status_timer_ = nullptr;
    } else {
        ESP_LOGI("Application", "Music status timer started (3s interval)");
    }
}
```

#### 4. 停止定时器

```cpp
void Application::StopMusicStatusTimer() {
    if (music_status_timer_) {
        esp_timer_stop(music_status_timer_);
        esp_timer_delete(music_status_timer_);
        music_status_timer_ = nullptr;
        ESP_LOGI("Application", "Music status timer stopped");
    }
}
```

#### 5. 在播放开始时启动定时器

在`HandleMusicSetPlaylist()`的末尾添加:

```cpp
void Application::HandleMusicSetPlaylist(const cJSON* data) {
    // ... 解析和设置播放列表 ...

    // 开始播放
    auto music = Board::GetInstance().GetMusic();
    const MusicItem* current = music_playlist_manager_->GetCurrentItem();
    if (music && current) {
        music->StartStreaming(current->url);

        // 启动状态上报定时器
        StartMusicStatusTimer();

        // 上报开始播放状态
        SendMusicStatus(true);
    }
}
```

#### 6. 在停止播放时停止定时器

在`HandleMusicControl()`的`stop`分支添加:

```cpp
} else if (action == "stop") {
    music->StopStreaming();

    // 停止状态上报定时器
    StopMusicStatusTimer();
}
```

---

## 构建配置修改

**文件**: `main/CMakeLists.txt`

添加新文件到编译列表:

```cmake
set(COMPONENT_SRCS
    # ... 现有文件 ...
    "boards/common/music_playlist_manager.cc"
)
```

---

## 测试计划

### 单元测试

#### 测试1: 顺序循环播放
```json
{
  "type": "music",
  "command": "set_playlist",
  "data": {
    "playlist_id": 100,
    "resource_type": 1,
    "playlist": [
      {"item_id": 101, "url": "http://server/1.mp3", "resource_name": "歌曲1", "duration": 180},
      {"item_id": 102, "url": "http://server/2.mp3", "resource_name": "歌曲2", "duration": 200},
      {"item_id": 103, "url": "http://server/3.mp3", "resource_name": "歌曲3", "duration": 220}
    ],
    "start_item_id": 101,
    "play_mode": 1,
    "play_type": 1
  }
}
```
**预期**: 按 1 → 2 → 3 → 1 → 2 → 3 无限循环播放

#### 测试2: 单次播放
```json
{
  "type": "music",
  "command": "set_playlist",
  "data": {
    "playlist_id": 125,
    "resource_type": 1,
    "playlist": [
      {"item_id": 601, "url": "http://server/1.mp3", "resource_name": "歌曲1", "duration": 180},
      {"item_id": 602, "url": "http://server/2.mp3", "resource_name": "歌曲2", "duration": 200}
    ],
    "start_item_id": 602,
    "play_mode": 1,
    "play_type": 2
  }
}
```
**预期**: 只播放歌曲2,播完停止

#### 测试3: 随机播放
```json
{
  "type": "music",
  "command": "set_playlist",
  "data": {
    "playlist_id": 124,
    "resource_type": 1,
    "playlist": [
      {"item_id": 501, "url": "http://server/1.mp3", "resource_name": "歌曲1", "duration": 180},
      {"item_id": 502, "url": "http://server/2.mp3", "resource_name": "歌曲2", "duration": 200},
      {"item_id": 503, "url": "http://server/3.mp3", "resource_name": "歌曲3", "duration": 220}
    ],
    "start_item_id": 501,
    "play_mode": 2,
    "play_type": 1
  }
}
```
**预期**: 随机播放,永不停止

#### 测试4: 单曲循环
```json
{
  "type": "music",
  "command": "set_playlist",
  "data": {
    "playlist_id": 123,
    "resource_type": 2,
    "playlist": [
      {"item_id": 456, "url": "http://server/1.mp3", "resource_name": "故事1", "duration": 180}
    ],
    "start_item_id": 456,
    "play_mode": 3,
    "play_type": 1
  }
}
```
**预期**: 反复播放故事1,永不停止

#### 测试5: 播放控制
```json
// 暂停
{"type": "music", "command": "control", "action": "pause"}

// 恢复
{"type": "music", "command": "control", "action": "resume"}

// 下一曲
{"type": "music", "command": "control", "action": "next"}

// 上一曲
{"type": "music", "command": "control", "action": "prev"}

// 停止
{"type": "music", "command": "control", "action": "stop"}
```

### 状态上报验证

1. **播放中**:每3秒上报一次,`playStatus=1`
2. **暂停时**:上报`playStatus=2`,停止定时器
3. **切歌时**:立即上报新的`itemId`
4. **停止时**:上报`playStatus=0`,停止定时器

---

## 调试清单

- [ ] MusicPlaylistManager编译通过
- [ ] Esp32Music头文件编译通过
- [ ] Esp32Music实现文件修改完成并编译通过
- [ ] Application头文件添加成员编译通过
- [ ] Application消息处理实现编译通过
- [ ] 设置播放列表功能测试通过
- [ ] 播放控制功能测试通过
- [ ] 状态上报定时器正常工作
- [ ] 播放完成回调正常触发
- [ ] 播放模式切换正常工作
- [ ] 内存无泄漏
- [ ] 线程无死锁

---

## 已知问题和注意事项

### 1. Esp32Music::PlayAudioStream() 播放时间追踪

当前实现没有精确追踪`currentPosition`(当前播放位置秒数)。需要:

```cpp
// 在PlayAudioStream()中维护播放时间
int64_t total_samples_played = 0;

// 每次播放一帧后:
total_samples_played += samples_in_frame;
current_position_sec = total_samples_played / sample_rate;
```

### 2. MusicPlaylistManager::OnSongFinished() 需要访问Application

当前设计中,`OnSongFinished()`调用回调时无法直接调用`Application::SendMusicStatus()`,需要:

**方案A**: 在回调中处理
```cpp
// 在Application::Start()中:
music_playlist_manager_->SetSongFinishedCallback([this]() {
    OnMusicSongFinished();  // 在这里调用SendMusicStatus
});
```

**方案B**: 修改MusicPlaylistManager,添加Application引用(不推荐,耦合度高)

### 3. 线程安全

- `MusicPlaylistManager`所有方法已加锁,线程安全
- `Application::SendMusicStatus()`从定时器和主线程调用,需确保线程安全
- 使用`Schedule()`将定时器回调调度到主线程执行(已实现)

### 4. 内存管理

- 确保所有`cJSON*`对象正确释放(`cJSON_Delete()`)
- 确保定时器正确停止和删除
- 播放列表可能很大,注意SPIRAM使用

---

## 下一步行动

1. **修改 esp32_music.cc**: 按照上述指南移除歌词相关代码
2. **实现 Application 消息处理**: 添加所有HandleMusic*方法
3. **实现状态上报**: 完成定时器和SendMusicStatus
4. **修改 CMakeLists.txt**: 添加music_playlist_manager.cc
5. **编译测试**: 确保无编译错误
6. **功能测试**: 按测试计划逐项验证
7. **调优**: 根据实际表现调整缓冲区大小、定时器间隔等

---

**文档结束**
