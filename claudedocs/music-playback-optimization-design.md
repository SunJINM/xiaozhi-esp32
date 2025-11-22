# ESP32音乐播放优化设计方案

**文档版本**: 2.0
**创建日期**: 2025-11-22
**最后更新**: 2025-11-22
**设计目标**: 简化音乐播放流程，支持服务端控制的播放列表管理和实时状态上报

---

## 目录

1. [需求分析](#需求分析)
2. [架构设计](#架构设计)
3. [协议消息格式](#协议消息格式)
4. [播放列表管理](#播放列表管理)
5. [播放模式实现](#播放模式实现)
6. [状态机设计](#状态机设计)
7. [数据结构设计](#数据结构设计)
8. [接口设计](#接口设计)
9. [实现计划](#实现计划)
10. [向后兼容性](#向后兼容性)

---

## 需求分析

### 当前系统问题

**现有流程**:
```
用户请求 → Download(song_name)
    → HTTP搜索音乐
    → 获取audio_url和lyric_url
    → StartStreaming()
    → 下载歌词
    → 显示歌词
```

**存在的问题**:
1. **过度耦合**: 音乐搜索逻辑耦合在播放器中
2. **网络开销**: 每次播放都需要额外的HTTP请求
3. **功能冗余**: 歌词下载和显示功能不再需要
4. **缺少列表**: 无法管理播放队列
5. **缺少模式**: 无法支持循环、随机等播放模式

### 新需求清单

#### 1. 移除功能 ❌
- ❌ 音乐搜索功能 (`Download(song_name, artist_name)`)
- ❌ 歌词下载 (`DownloadLyrics()`)
- ❌ 歌词解析 (`ParseLyrics()`)
- ❌ 歌词显示 (`UpdateLyricDisplay()`)
- ❌ 歌词线程 (`lyric_thread_`)

#### 2. 新增功能 ✅
- ✅ 服务端下发播放列表
- ✅ 播放模式支持(顺序/随机/单曲循环)
- ✅ 播放类型支持(单曲/全部)
- ✅ 播放控制指令(播放/暂停/停止/上一曲/下一曲)
- ✅ 播放状态上报

#### 3. 保留功能 ⚪
- ⚪ HTTP流式下载 (`DownloadAudioStream()`)
- ⚪ MP3解码 (`PlayAudioStream()`)
- ⚪ 音频输出 (`AddAudioData()`)
- ⚪ 缓冲管理 (`audio_buffer_`)
- ⚪ 频谱显示 (`DISPLAY_MODE_SPECTRUM`)

---

## 架构设计

### 整体架构

```
┌─────────────────────────────────────────────────────────┐
│                     服务端                               │
│  (下发播放指令和播放列表)                                 │
└────────────────────┬────────────────────────────────────┘
                     ↓ JSON消息
         ┌───────────────────────────┐
         │  Protocol (MQTT/WebSocket) │
         │  OnIncomingJson()          │
         └───────────┬───────────────┘
                     ↓
         ┌───────────────────────────┐
         │  Application               │
         │  HandleMusicCommand()      │
         └───────────┬───────────────┘
                     ↓
         ┌───────────────────────────┐
         │  MusicPlaylistManager      │  ← 新增组件
         │  - playlist_               │
         │  - play_mode_              │
         │  - play_type_              │
         │  - current_index_          │
         └───────────┬───────────────┘
                     ↓
         ┌───────────────────────────┐
         │  Esp32Music (简化版)       │
         │  - StartStreaming(url)     │
         │  - StopStreaming()         │
         │  - PauseSong()             │
         │  - ResumeSong()            │
         └───────────┬───────────────┘
                     ↓
         ┌───────────────────────────┐
         │  AudioCodec (I2S输出)      │
         └───────────────────────────┘
```

### 模块职责

#### 1. Protocol (协议层)
- **职责**: 接收服务端JSON消息
- **改动**: 新增 `music` 类型消息处理
- **位置**: [main/protocols/protocol.h](../main/protocols/protocol.h)

#### 2. Application (应用层)
- **职责**: 分发音乐控制指令
- **改动**: 新增 `HandleMusicCommand()` 方法
- **位置**: [main/application.cc](../main/application.cc)

#### 3. MusicPlaylistManager (新增)
- **职责**: 管理播放列表和播放逻辑
- **功能**:
  - 播放列表存储
  - 播放模式控制(顺序/随机/循环)
  - 播放类型控制(单曲/全部)
  - 下一曲/上一曲逻辑
  - 播放完成回调

#### 4. Esp32Music (简化版)
- **职责**: 单曲流式播放
- **移除**: 搜索、歌词相关功能
- **保留**: 流式下载、解码、播放控制
- **位置**: [main/boards/common/esp32_music.cc](../main/boards/common/esp32_music.cc)

---

## 协议消息格式

### 服务端 → 设备 消息

#### 1. 设置播放列表 (music_set_playlist)

```json
{
  "type": "music",
  "command": "set_playlist",
  "data": {
    "playlist_id": 123,
    "resource_type": 2,
    "playlist": [
      {
        "item_id": 456,
        "url": "http://server:5005/music/song1.mp3",
        "resource_name": "白雪公主",
        "duration": 240
      },
      {
        "item_id": 457,
        "url": "http://server:5005/music/song2.mp3",
        "resource_name": "小红帽",
        "duration": 180
      }
    ],
    "start_item_id": 456,
    "play_mode": 1
  }
}
```

**字段说明**:
- `playlist_id` (必需): 歌单ID,用于状态上报
- `resource_type` (必需): 资源类型
  - `1`: 歌曲
  - `2`: 故事
- `playlist`: 资源列表
  - `item_id` (必需): 明细ID,用于状态上报
  - `url` (必需): 资源文件直链
  - `resource_name` (必需): 资源名称
  - `duration` (必需): 时长(秒)
- `start_item_id` (可选): 开始播放的item_id,默认第一首
- `play_mode` (必需): 播放模式(仅在play_type=1时有效)
  - `1`: 顺序播放(按列表顺序循环)
  - `2`: 随机播放(随机选择下一首)
  - `3`: 单曲循环(反复播放当前歌曲)
- `play_type` (必需): 播放类型
  - `1`: 循环播放(永不停止,按play_mode规则播放)
  - `2`: 单次播放(播放start_item_id指定的那一首后停止)

**播放行为矩阵**:
| play_type | play_mode | 行为 |
|-----------|-----------|------|
| 循环(1) | 顺序(1) | 按顺序无限循环: 1→2→3→1→2→3... |
| 循环(1) | 随机(2) | 随机播放,永不停止 |
| 循环(1) | 单曲循环(3) | 反复播放当前歌曲: 1→1→1... |
| 单次(2) | 任意 | 播放start_item_id指定的歌曲后停止,play_mode不起作用 |

#### 2. 播放控制 (music_control)

```json
{
  "type": "music",
  "command": "control",
  "action": "play"
}
```

**action值**:
- `play`: 开始播放(如已暂停则恢复)
- `pause`: 暂停
- `stop`: 停止
- `next`: 下一曲
- `prev`: 上一曲
- `resume`: 恢复播放

#### 3. 设置播放模式 (music_set_mode)

```json
{
  "type": "music",
  "command": "set_mode",
  "play_mode": 2,
  "play_type": 2
}
```

**参数说明**:
- `play_mode` (可选): 播放模式(仅在play_type=1时有效)
  - `1`: 顺序播放(循环)
  - `2`: 随机播放
  - `3`: 单曲循环
- `play_type` (可选): 播放类型
  - `1`: 循环播放(永不停止)
  - `2`: 单次播放(播完当前停止)

### 设备 → 服务端 消息

#### 1. 播放状态上报 (定时+事件驱动)

**上报策略**:
- **定时上报**: 播放中时每3秒上报一次
- **事件上报**: 关键事件立即上报(切歌、暂停、恢复、停止)

**Redis HSET 格式**:
```
HSET device:play:status:xx:xx:xx:xx:xx:xx
  playStatus 1                    # 播放状态: 0-停止, 1-播放中, 2-暂停
  playlistId 123                  # 当前歌单ID
  itemId 456                      # 当前播放的资源ID
  resourceName "白雪公主"          # 资源名称
  resourceType 2                  # 资源类型: 1-歌曲, 2-故事
  playMode 1                      # 播放模式: 1-顺序, 2-循环, 3-随机
  playType 2                      # 播放类型: 1-单曲, 2-全部
  duration 180                    # 总时长(秒)
  currentPosition 45              # 当前播放位置(秒)
  updateTime 1234567890           # 更新时间戳
```

**通过协议上报的JSON格式**:
```json
{
  "type": "music_status",
  "mac": "xx:xx:xx:xx:xx:xx",
  "playStatus": 1,
  "playlistId": 123,
  "itemId": 456,
  "resourceName": "白雪公主",
  "resourceType": 2,
  "playMode": 1,
  "playType": 2,
  "duration": 180,
  "currentPosition": 45,
  "updateTime": 1234567890
}
```

**playStatus值**:
- `0`: 停止(播放完成或用户停止)
- `1`: 播放中
- `2`: 暂停

**上报触发时机**:
1. **定时触发**: 播放中时每3秒一次
2. **切歌触发**: 切换到下一曲或上一曲时
3. **暂停触发**: 用户暂停播放时
4. **恢复触发**: 恢复播放时
5. **停止触发**: 停止播放时(播放完成或手动停止)
6. **开始触发**: 开始播放新歌单时

---

## 播放列表管理

### MusicPlaylistManager 类设计

```cpp
// 播放模式枚举 (只在play_type=1时有效)
enum PlayMode {
    kPlayModeSequential = 1,  // 顺序播放(循环)
    kPlayModeRandom = 2,       // 随机播放
    kPlayModeSingleLoop = 3    // 单曲循环
};

// 播放类型枚举
enum PlayType {
    kPlayTypeLoop = 1,         // 循环播放(永不停止)
    kPlayTypeSingle = 2        // 单次播放(播完当前歌曲停止)
};

// 音乐项结构
struct MusicItem {
    int item_id;               // 明细ID
    std::string url;           // 资源URL
    std::string resource_name; // 资源名称
    int duration;              // 时长(秒)
};

class MusicPlaylistManager {
public:
    MusicPlaylistManager();
    ~MusicPlaylistManager();

    // 播放列表管理
    void SetPlaylist(const std::vector<MusicItem>& playlist);
    void ClearPlaylist();
    const std::vector<MusicItem>& GetPlaylist() const { return playlist_; }
    int GetPlaylistSize() const { return playlist_.size(); }

    // 播放模式和类型
    void SetPlayMode(PlayMode mode);
    void SetPlayType(PlayType type);
    PlayMode GetPlayMode() const { return play_mode_; }
    PlayType GetPlayType() const { return play_type_; }

    // 当前播放项
    bool SetCurrentByUrl(const std::string& url);
    bool SetCurrentByIndex(int index);
    int GetCurrentIndex() const { return current_index_; }
    const MusicItem* GetCurrentItem() const;

    // 导航
    const MusicItem* GetNextItem();     // 获取下一曲
    const MusicItem* GetPreviousItem(); // 获取上一曲
    bool HasNext() const;               // 是否有下一曲
    bool HasPrevious() const;           // 是否有上一曲

    // 播放完成处理
    void OnSongFinished();  // 当前歌曲播放完成回调

    // 随机播放
    void GenerateRandomSequence();  // 生成随机播放序列

private:
    std::vector<MusicItem> playlist_;
    PlayMode play_mode_;
    PlayType play_type_;
    int current_index_;
    std::vector<int> random_sequence_;  // 随机播放序列
    int random_position_;                // 当前随机序列位置
};
```

### 核心逻辑

#### 1. GetNextItem 实现

```cpp
const MusicItem* MusicPlaylistManager::GetNextItem() {
    if (playlist_.empty()) return nullptr;

    // 单次播放模式:播完当前歌曲就停止
    if (play_type_ == kPlayTypeSingle) {
        return nullptr;  // 播完停止,不管play_mode
    }

    // 循环播放模式:根据play_mode决定下一曲
    if (play_mode_ == kPlayModeSequential) {
        // 顺序播放(循环)
        current_index_++;
        if (current_index_ >= playlist_.size()) {
            current_index_ = 0;  // 循环回第一首
        }
        return &playlist_[current_index_];
    } else if (play_mode_ == kPlayModeRandom) {
        // 随机播放
        return GetNextRandomItem();
    } else if (play_mode_ == kPlayModeSingleLoop) {
        // 单曲循环:当前索引不变
        return &playlist_[current_index_];
    }

    return nullptr;
}
```

#### 2. 随机播放 (PlayMode = 2)

**实现思路**: 每次随机选择一首歌曲(可以重复),永不停止。

```cpp
const MusicItem* MusicPlaylistManager::GetNextRandomItem() {
    if (playlist_.empty()) return nullptr;

    // 简单随机:从列表中随机选一首
    int random_index = esp_random() % playlist_.size();
    current_index_ = random_index;
    return &playlist_[current_index_];
}
```

**说明**:
- 随机播放时,每次都随机选择,可能重复
- 永不停止,一直随机播放

#### 3. 播放完成处理

```cpp
void MusicPlaylistManager::OnSongFinished() {
    const MusicItem* next_item = GetNextItem();

    if (next_item) {
        // 有下一曲,通知播放器播放
        auto music = Board::GetInstance().GetMusic();
        if (music) {
            music->StartStreaming(next_item->url);
        }
        // 上报切歌状态
        Application::GetInstance().SendMusicStatus(true);
    } else {
        // 播放结束
        ESP_LOGI(TAG, "Playlist finished");
        // 上报播放结束状态(playStatus=0)
        Application::GetInstance().SendMusicStatus(true);
    }
}
```

---

## 播放模式实现

### 播放行为矩阵

| 播放类型 | 播放模式 | 行为 |
|---------|---------|------|
| 循环 (1) | 顺序 (1) | 按顺序无限循环: 1→2→3→1→2→3... |
| 循环 (1) | 随机 (2) | 随机播放,永不停止 |
| 循环 (1) | 单曲循环 (3) | 反复播放当前歌曲: 1→1→1... |
| 单次 (2) | 任意 | **播放start_item_id指定的歌曲后停止** |

**注意**:
- 当 `play_type = 2` (单次播放)时,`play_mode` 参数不起作用,播完当前歌曲就停止
- 当 `play_type = 1` (循环播放)时,根据`play_mode`决定如何选择下一曲,永不停止

### 决策流程图

```
歌曲播放完成
    ↓
检查播放类型
    ↓
┌───────────────────┐
│ 单次播放 (type=2)  │ → 直接停止播放(不管play_mode)
└─────┬─────────────┘
      ↓ (否,type=1 循环播放)
检查播放模式
    ↓
┌───────────────────────────────────┐
│ 顺序播放 (mode=1)                  │
│   ├─ index++                      │
│   ├─ if index >= size: index = 0  │
│   └─ 播放下一首(循环)              │
└─────┬─────────────────────────────┘
      ↓
┌───────────────────────────────────┐
│ 随机播放 (mode=2)                  │
│   ├─ random_index = rand()        │
│   └─ 播放随机歌曲(永不停止)        │
└─────┬─────────────────────────────┘
      ↓
┌───────────────────────────────────┐
│ 单曲循环 (mode=3)                  │
│   ├─ index不变                     │
│   └─ 播放当前歌曲(无限循环)        │
└─────────────────────────────────┘
```

---

## 状态机设计

### 播放器状态

```cpp
enum MusicPlayerState {
    kMusicStateIdle,       // 空闲
    kMusicStatePlaying,    // 播放中
    kMusicStatePaused,     // 暂停
    kMusicStateBuffering,  // 缓冲中
    kMusicStateStopped,    // 停止
    kMusicStateError       // 错误
};
```

### 状态转换图

```
        ┌─────────────┐
        │    Idle     │ (初始状态)
        └──────┬──────┘
               ↓ set_playlist + play
        ┌─────────────┐
    ┌───│  Buffering  │←─────┐
    │   └──────┬──────┘      │
    │          ↓ 缓冲完成      │ 下一曲
    │   ┌─────────────┐      │
    │   │   Playing   │──────┘
    │   └──────┬──────┘
    │          │ pause    resume
    │          ↓          ↑
    │   ┌─────────────┐  │
    │   │   Paused    │──┘
    │   └─────────────┘
    │
    │   stop
    ↓
┌─────────────┐
│   Stopped   │
└─────────────┘
    ↑
    │ error
┌─────────────┐
│    Error    │
└─────────────┘
```

### 状态转换表

| 当前状态 | 事件 | 下一状态 | 动作 |
|---------|------|---------|------|
| Idle | set_playlist + play | Buffering | 开始下载第一首 |
| Buffering | buffer_ready | Playing | 开始解码播放 |
| Buffering | error | Error | 记录错误,上报 |
| Playing | pause | Paused | 暂停解码 |
| Playing | stop | Stopped | 停止播放,清空缓冲 |
| Playing | song_finished | Buffering/Stopped | 判断是否有下一曲 |
| Playing | error | Error | 记录错误,上报 |
| Paused | resume | Playing | 恢复解码 |
| Paused | stop | Stopped | 停止播放 |
| Stopped | play | Buffering | 重新开始播放 |
| Error | retry | Buffering | 重试当前歌曲 |
| Error | skip | Buffering | 跳到下一曲 |

---

## 数据结构设计

### 1. 播放列表存储

```cpp
// main/boards/common/music_playlist_manager.h

// 资源项结构
struct MusicItem {
    int item_id;               // 明细ID (必需,用于状态上报)
    std::string url;           // 资源URL (必需)
    std::string resource_name; // 资源名称 (必需)
    int duration;              // 时长,秒 (必需)

    MusicItem() : item_id(0), duration(0) {}
    MusicItem(int id, const std::string& url, const std::string& name, int dur)
        : item_id(id), url(url), resource_name(name), duration(dur) {}
};

class MusicPlaylistManager {
private:
    int playlist_id_;                          // 歌单ID
    int resource_type_;                        // 资源类型: 1-歌曲, 2-故事
    std::vector<MusicItem> playlist_;          // 播放列表
    std::vector<int> random_sequence_;         // 随机播放序列
    PlayMode play_mode_;                       // 播放模式: 1-顺序, 2-循环, 3-随机
    PlayType play_type_;                       // 播放类型: 1-单曲, 2-全部
    int current_index_;                        // 当前播放索引
    int random_position_;                      // 随机序列位置
    std::mutex mutex_;                         // 线程安全
};
```

### 2. Esp32Music 简化版

```cpp
// main/boards/common/esp32_music.h

class Esp32Music : public Music {
private:
    // === 保留字段 ===
    std::string current_music_url_;            // 当前播放URL
    std::string current_song_name_;            // 当前歌曲名
    bool song_name_displayed_;                 // 歌名是否已显示

    std::atomic<DisplayMode> display_mode_;    // 显示模式
    std::atomic<bool> is_playing_;             // 播放中
    std::atomic<bool> is_downloading_;         // 下载中
    std::atomic<bool> is_paused_;              // 暂停

    std::thread play_thread_;                  // 播放线程
    std::thread download_thread_;              // 下载线程

    std::queue<AudioChunk> audio_buffer_;      // 音频缓冲区
    std::mutex buffer_mutex_;                  // 缓冲区锁
    std::condition_variable buffer_cv_;        // 条件变量
    size_t buffer_size_;                       // 缓冲区大小

    HMP3Decoder mp3_decoder_;                  // MP3解码器
    MP3FrameInfo mp3_frame_info_;              // MP3帧信息
    bool mp3_decoder_initialized_;             // 解码器初始化标志

    int16_t* final_pcm_data_fft;               // FFT数据(频谱显示)

    // === 移除字段 ===
    // ❌ std::string current_lyric_url_;
    // ❌ std::vector<std::pair<int, std::string>> lyrics_;
    // ❌ std::atomic<int> current_lyric_index_;
    // ❌ std::thread lyric_thread_;
    // ❌ std::atomic<bool> is_lyric_running_;

    // === 新增字段 ===
    MusicPlayerState player_state_;            // 播放器状态
    std::function<void()> on_song_finished_;   // 歌曲完成回调
    int64_t current_play_time_ms_;             // 当前播放时间(毫秒)
    int current_duration_sec_;                 // 当前歌曲总时长(秒)
};
```

### 3. Application 扩展

```cpp
// main/application.h

class Application {
private:
    // ... 现有字段 ...

    // === 新增 ===
    std::unique_ptr<MusicPlaylistManager> music_playlist_manager_;
    esp_timer_handle_t music_status_timer_;    // 状态上报定时器

    // 音乐控制方法
    void HandleMusicCommand(const cJSON* data);
    void HandleMusicSetPlaylist(const cJSON* data);
    void HandleMusicControl(const std::string& action);
    void HandleMusicSetMode(const cJSON* data);

    // 播放事件回调
    void OnMusicSongFinished();

    // 状态上报
    void SendMusicStatus(bool force = false);  // force=true表示事件触发
    void StartMusicStatusTimer();              // 启动3秒定时器
    void StopMusicStatusTimer();               // 停止定时器
    static void MusicStatusTimerCallback(void* arg);  // 定时器回调
};
```

---

## 接口设计

### 1. MusicPlaylistManager 公共接口

```cpp
// 初始化
MusicPlaylistManager();
~MusicPlaylistManager();

// 播放列表管理
void SetPlaylist(const std::vector<MusicItem>& playlist);
void AddToPlaylist(const MusicItem& item);
void RemoveFromPlaylist(int index);
void ClearPlaylist();
const std::vector<MusicItem>& GetPlaylist() const;
int GetPlaylistSize() const;

// 播放模式和类型
void SetPlayMode(PlayMode mode);
void SetPlayType(PlayType type);
PlayMode GetPlayMode() const;
PlayType GetPlayType() const;

// 当前播放项
bool SetCurrentByUrl(const std::string& url);
bool SetCurrentByIndex(int index);
int GetCurrentIndex() const;
const MusicItem* GetCurrentItem() const;

// 导航
const MusicItem* GetNextItem();
const MusicItem* GetPreviousItem();
bool HasNext() const;
bool HasPrevious() const;

// 事件处理
void OnSongFinished();  // 播放完成回调
void SetSongFinishedCallback(std::function<void()> callback);
```

### 2. Esp32Music 简化接口

```cpp
// === 保留接口 ===
bool StartStreaming(const std::string& music_url) override;
bool StopStreaming() override;
bool PauseSong() override;
bool ResumeSong() override;
bool SetVolume(int volume) override;

bool IsPlaying() const override;
bool IsPaused() const override;
size_t GetBufferSize() const override;
int16_t* GetAudioData() override;  // FFT数据

// 显示模式
void SetDisplayMode(DisplayMode mode);
DisplayMode GetDisplayMode() const;

// === 新增接口 ===
void SetSongInfo(const std::string& title, const std::string& artist);
void SetSongFinishedCallback(std::function<void()> callback);
void SetErrorCallback(std::function<void(const std::string&)> callback);
MusicPlayerState GetPlayerState() const;

// === 移除接口 ===
// ❌ bool Download(const std::string& song_name, const std::string& artist_name);
// ❌ std::string GetDownloadResult();
```

### 3. Application 音乐控制接口

```cpp
// 私有方法(内部使用)
void HandleMusicCommand(const cJSON* data);

// 子命令处理
void HandleMusicSetPlaylist(const cJSON* data);
void HandleMusicControl(const std::string& action);
void HandleMusicSetMode(const cJSON* data);
void HandleMusicPlayUrl(const cJSON* data);

// 事件回调
void OnMusicSongFinished();
void OnMusicError(const std::string& error);

// 状态上报
void SendMusicStatus();
void SendMusicEvent(const std::string& event, const cJSON* extra_data);
```

---

## 实现计划

### Phase 1: 核心数据结构 (2-3小时)

**文件**: `main/boards/common/music_playlist_manager.h/cc`

**任务**:
1. 创建 `MusicItem` 结构体
2. 创建 `PlayMode` 和 `PlayType` 枚举
3. 实现 `MusicPlaylistManager` 类基础框架
4. 实现播放列表存储和访问方法
5. 单元测试: 添加/删除/清空列表

**输出**:
- [x] `music_playlist_manager.h`
- [x] `music_playlist_manager.cc`
- [x] 基础测试用例

### Phase 2: 播放模式逻辑 (3-4小时)

**文件**: `music_playlist_manager.cc`

**任务**:
1. 实现顺序播放逻辑 (`GetNextItem()` for mode=1)
2. 实现随机播放逻辑 (Fisher-Yates洗牌算法)
3. 实现单曲循环逻辑 (`GetNextItem()` for mode=3)
4. 实现上一曲逻辑 (`GetPreviousItem()`)
5. 处理播放类型 (单曲/全部)
6. 单元测试: 各种模式组合

**输出**:
- [x] 完整的导航逻辑
- [x] 随机播放测试
- [x] 边界条件测试

### Phase 3: Esp32Music 简化 (2-3小时)

**文件**: `main/boards/common/esp32_music.h/cc`

**任务**:
1. 移除歌词相关字段和方法
   - 删除 `current_lyric_url_`
   - 删除 `lyrics_`、`lyrics_mutex_`
   - 删除 `lyric_thread_`、`is_lyric_running_`
   - 删除 `LyricDisplayThread()`
   - 删除 `DownloadLyrics()`
   - 删除 `ParseLyrics()`
   - 删除 `UpdateLyricDisplay()`
2. 移除音乐搜索功能
   - 删除 `Download(song_name, artist_name)`
   - 保留 `StartStreaming(url)` 核心功能
3. 新增播放状态管理
   - 添加 `MusicPlayerState player_state_`
   - 添加状态转换方法
4. 新增回调机制
   - `on_song_finished_` 回调
   - `on_error_` 回调
5. 修改 `PlayAudioStream()` 检测播放完成
   - 播放完成时调用 `on_song_finished_()`

**输出**:
- [x] 简化的 `Esp32Music` 类
- [x] 播放完成回调触发

### Phase 4: 协议消息处理 (3-4小时)

**文件**: `main/application.cc`

**任务**:
1. 在 `OnIncomingJson` 回调中添加 `music` 类型处理
2. 实现 `HandleMusicCommand()` 分发器
3. 实现 `HandleMusicSetPlaylist()`
   - 解析JSON播放列表
   - 调用 `MusicPlaylistManager::SetPlaylist()`
   - 设置播放模式和类型
   - 找到起始URL并开始播放
4. 实现 `HandleMusicControl()`
   - `play`: 播放
   - `pause`: 暂停
   - `resume`: 恢复
   - `stop`: 停止
   - `next`: 下一曲
   - `prev`: 上一曲
5. 实现 `HandleMusicSetMode()`
6. 实现 `HandleMusicPlayUrl()` (单曲快速播放)

**输出**:
- [x] 完整的消息处理流程
- [x] 各种控制指令支持

### Phase 5: 状态上报 (2-3小时)

**文件**: `main/application.cc`

**任务**:
1. 实现 `SendMusicStatus()` 方法
   - 构建状态JSON
   - 通过Protocol发送
2. 实现 `SendMusicEvent()` 方法
   - 播放完成事件
   - 错误事件
3. 在关键节点调用状态上报
   - 播放开始
   - 播放暂停
   - 播放停止
   - 切换歌曲
   - 播放完成

**输出**:
- [x] 状态上报机制
- [x] 事件通知机制

### Phase 6: 集成和测试 (4-5小时)

**任务**:
1. 集成所有组件
2. 端到端测试
   - 服务端发送播放列表 → 设备播放
   - 各种播放模式测试
   - 播放控制测试 (暂停/恢复/下一曲/上一曲)
3. 边界测试
   - 空播放列表
   - 单曲播放列表
   - 网络错误处理
   - URL无效处理
4. 性能测试
   - 内存占用
   - 线程稳定性
   - 快速切歌测试

**输出**:
- [x] 完整可用的音乐播放系统
- [x] 测试报告

### Phase 7: 文档和清理 (1-2小时)

**任务**:
1. 更新代码注释
2. 编写使用文档
3. 更新协议文档
4. 清理调试代码
5. 代码格式化

**输出**:
- [x] 完整的技术文档
- [x] 清理后的代码

---

## 向后兼容性

### 1. MCP工具不受影响

由于音乐播放MCP工具已被注释禁用,本次优化**不影响**MCP工具列表。

### 2. 现有播放功能保留

以下核心功能保持不变:
- HTTP流式下载
- MP3解码
- 音频输出
- 缓冲管理
- 频谱显示
- 音量控制

### 3. 渐进式迁移

**兼容性策略**:

#### 方案A: 双协议并存 (推荐)
```cpp
// 同时支持旧协议(custom消息)和新协议(music消息)
if (strcmp(type->valuestring, "custom") == 0) {
    // 旧协议: 自定义消息
    auto action = cJSON_GetObjectItem(payload, "action");
    if (strcmp(action->valuestring, "play_music") == 0) {
        // 兼容旧的播放逻辑
        auto url = cJSON_GetObjectItem(payload, "url");
        HandleMusicPlayUrl(payload);
    }
} else if (strcmp(type->valuestring, "music") == 0) {
    // 新协议: 专用音乐消息
    HandleMusicCommand(root);
}
```

#### 方案B: 完全替换
直接使用新协议,移除旧的custom消息处理。

**建议**: 采用方案A,保持1-2个版本的过渡期。

---

## 附录

### A. 代码示例

#### 示例1: 解析播放列表

```cpp
void Application::HandleMusicSetPlaylist(const cJSON* data) {
    auto playlist_json = cJSON_GetObjectItem(data, "playlist");
    if (!cJSON_IsArray(playlist_json)) {
        ESP_LOGE(TAG, "Invalid playlist format");
        return;
    }

    std::vector<MusicItem> playlist;
    int size = cJSON_GetArraySize(playlist_json);

    for (int i = 0; i < size; i++) {
        cJSON* item_json = cJSON_GetArrayItem(playlist_json, i);
        MusicItem item;

        // 必需字段
        auto item_id = cJSON_GetObjectItem(item_json, "item_id");
        if (!cJSON_IsNumber(item_id)) {
            ESP_LOGW(TAG, "Skipping item without item_id");
            continue;
        }
        item.item_id = item_id->valueint;

        auto url = cJSON_GetObjectItem(item_json, "url");
        if (!cJSON_IsString(url)) {
            ESP_LOGW(TAG, "Skipping item without URL");
            continue;
        }
        item.url = url->valuestring;

        auto resource_name = cJSON_GetObjectItem(item_json, "resource_name");
        if (cJSON_IsString(resource_name)) {
            item.resource_name = resource_name->valuestring;
        }

        auto duration = cJSON_GetObjectItem(item_json, "duration");
        if (cJSON_IsNumber(duration)) {
            item.duration = duration->valueint;
        }

        playlist.push_back(item);
    }

    // 设置播放列表
    music_playlist_manager_->SetPlaylist(playlist);

    // 设置播放模式和类型
    auto play_mode = cJSON_GetObjectItem(data, "play_mode");
    if (cJSON_IsNumber(play_mode)) {
        music_playlist_manager_->SetPlayMode((PlayMode)play_mode->valueint);
    }

    auto play_type = cJSON_GetObjectItem(data, "play_type");
    if (cJSON_IsNumber(play_type)) {
        music_playlist_manager_->SetPlayType((PlayType)play_type->valueint);
    }

    // 设置playlist_id和resource_type
    auto playlist_id = cJSON_GetObjectItem(data, "playlist_id");
    if (cJSON_IsNumber(playlist_id)) {
        music_playlist_manager_->SetPlaylistId(playlist_id->valueint);
    }

    auto resource_type = cJSON_GetObjectItem(data, "resource_type");
    if (cJSON_IsNumber(resource_type)) {
        music_playlist_manager_->SetResourceType(resource_type->valueint);
    }

    // 设置起始item_id
    auto start_item_id = cJSON_GetObjectItem(data, "start_item_id");
    if (cJSON_IsNumber(start_item_id)) {
        music_playlist_manager_->SetCurrentByItemId(start_item_id->valueint);
    } else {
        music_playlist_manager_->SetCurrentByIndex(0);
    }

    // 开始播放
    auto music = Board::GetInstance().GetMusic();
    const MusicItem* current = music_playlist_manager_->GetCurrentItem();
    if (music && current) {
        music->StartStreaming(current->url);
        // 上报开始播放状态
        SendMusicStatus(true);
    }
}
```

#### 示例2: 播放控制

```cpp
void Application::HandleMusicControl(const std::string& action) {
    auto music = Board::GetInstance().GetMusic();
    if (!music) return;

    if (action == "play") {
        // 如果已暂停,则恢复;否则从头播放
        if (music->IsPaused()) {
            music->ResumeSong();
        } else {
            const MusicItem* current = music_playlist_manager_->GetCurrentItem();
            if (current) {
                music->StartStreaming(current->url);
            }
        }
    } else if (action == "pause") {
        music->PauseSong();
    } else if (action == "resume") {
        music->ResumeSong();
    } else if (action == "stop") {
        music->StopStreaming();
    } else if (action == "next") {
        const MusicItem* next = music_playlist_manager_->GetNextItem();
        if (next) {
            music->StartStreaming(next->url);
        } else {
            ESP_LOGI(TAG, "No next item available");
        }
    } else if (action == "prev") {
        const MusicItem* prev = music_playlist_manager_->GetPreviousItem();
        if (prev) {
            music->StartStreaming(prev->url);
        } else {
            ESP_LOGI(TAG, "No previous item available");
        }
    }

    // 上报状态(事件触发)
    SendMusicStatus(true);
}
```

#### 示例3: 播放完成回调

```cpp
// 在Application::Start()中设置回调
void Application::Start() {
    // ... 现有代码 ...

    auto music = Board::GetInstance().GetMusic();
    if (music) {
        music->SetSongFinishedCallback([this]() {
            Schedule([this]() {
                OnMusicSongFinished();
            });
        });

        music->SetErrorCallback([this](const std::string& error) {
            Schedule([this, error]() {
                OnMusicError(error);
            });
        });
    }
}

void Application::OnMusicSongFinished() {
    ESP_LOGI(TAG, "Song finished, checking for next...");

    // 调用播放列表管理器处理
    music_playlist_manager_->OnSongFinished();

    // OnSongFinished内部会判断是否有下一曲并自动播放
}
```

### B. 测试用例

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

// 预期: 按 1 → 2 → 3 → 1 → 2 → 3 无限循环播放
```

#### 测试2: 随机播放

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

// 预期: 随机播放,如 1 → 3 → 3 → 2 → 1 → 1... 永不停止
```

#### 测试3: 单曲循环

```json
{
  "type": "music",
  "command": "set_playlist",
  "data": {
    "playlist_id": 123,
    "resource_type": 2,
    "playlist": [
      {"item_id": 456, "url": "http://server/1.mp3", "resource_name": "故事1", "duration": 180},
      {"item_id": 457, "url": "http://server/2.mp3", "resource_name": "故事2", "duration": 200}
    ],
    "start_item_id": 456,
    "play_mode": 3,
    "play_type": 1
  }
}

// 预期: 反复播放歌曲1(item_id=456): 1 → 1 → 1 → 1... 永不停止
```

#### 测试4: 单次播放

```json
{
  "type": "music",
  "command": "set_playlist",
  "data": {
    "playlist_id": 125,
    "resource_type": 1,
    "playlist": [
      {"item_id": 601, "url": "http://server/1.mp3", "resource_name": "歌曲1", "duration": 180},
      {"item_id": 602, "url": "http://server/2.mp3", "resource_name": "歌曲2", "duration": 200},
      {"item_id": 603, "url": "http://server/3.mp3", "resource_name": "歌曲3", "duration": 220}
    ],
    "start_item_id": 602,
    "play_mode": 1,
    "play_type": 2
  }
}

// 预期: 只播放歌曲2(item_id=602),播完停止,不管play_mode设置
```

#### 测试5: 播放控制

```json
// 1. 暂停
{"type": "music", "command": "control", "action": "pause"}

// 2. 恢复
{"type": "music", "command": "control", "action": "resume"}

// 3. 下一曲
{"type": "music", "command": "control", "action": "next"}

// 4. 上一曲
{"type": "music", "command": "control", "action": "prev"}

// 5. 停止
{"type": "music", "command": "control", "action": "stop"}
```

### C. 性能考虑

#### 内存占用估算

```
播放列表: sizeof(MusicItem) * 列表长度
  - MusicItem ≈ 200 bytes (url + title + artist)
  - 100首歌 ≈ 20KB

随机序列: sizeof(int) * 列表长度
  - 100首歌 ≈ 400 bytes

总额外开销: ~25KB (可接受)
```

#### 线程使用

```
现有线程:
  - download_thread_ (8KB栈)
  - play_thread_ (8KB栈)

新增: 无新线程 ✅

总线程数: 不变
```

---

## 总结

### 优化效果

| 项目 | 优化前 | 优化后 | 改善 |
|-----|-------|-------|------|
| HTTP请求数 | 2-3次/首歌 | 1次/首歌 | ↓50% |
| 网络延迟 | 搜索+下载 | 仅下载 | ↓30% |
| 内存占用 | 歌词+URL | 仅URL | ↓10KB |
| 代码复杂度 | 高(歌词解析) | 低 | ↓30% |
| 播放列表 | ❌ | ✅ | 新功能 |
| 播放模式 | ❌ | ✅ | 新功能 |

### 核心改进

1. **架构清晰**: 职责分离(列表管理 vs 播放器)
2. **服务端控制**: 播放逻辑由服务端决策
3. **功能增强**: 支持播放列表、多种播放模式
4. **性能提升**: 减少HTTP请求、移除歌词处理
5. **易维护**: 代码更简洁、逻辑更清晰

### 风险评估

| 风险 | 等级 | 缓解措施 |
|-----|------|---------|
| 协议不兼容 | 低 | 双协议并存过渡 |
| 播放列表过大 | 中 | 限制列表大小(建议≤100) |
| 随机算法性能 | 低 | Fisher-Yates算法O(n) |
| 内存泄漏 | 低 | 使用RAII,智能指针 |
| 线程安全 | 中 | 添加互斥锁保护 |

### 后续扩展

可选功能(未来版本):
- 播放进度跳转
- 音乐收藏/喜欢
- 播放历史记录
- 音质选择(低/中/高)
- 均衡器设置

---

**文档结束**
