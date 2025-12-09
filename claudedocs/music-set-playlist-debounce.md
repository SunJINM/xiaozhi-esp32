# 音乐播放列表指令防抖优化

## 问题描述

在音乐播放功能中，如果短时间内多次下发 `HandleMusicSetPlaylist` 指令，会导致 ESP32 设备崩溃。这是因为：

1. 播放列表设置操作涉及大量内存分配和网络请求
2. 频繁的播放列表切换会导致资源竞争和内存泄漏
3. 设备在处理上一个播放列表时，又收到新的播放列表指令，导致状态混乱

## 解决方案

实现防抖（Debounce）机制，过滤掉短时间内重复的 `HandleMusicSetPlaylist` 指令。

### 实现细节

#### 1. 添加防抖相关成员变量

在 `application.h` 的 `Application` 类私有成员中添加：

```cpp
// 防抖相关：防止短时间内重复下发 set_playlist 指令
int64_t last_set_playlist_time_ = 0;  // 上次执行 set_playlist 的时间戳（微秒）
static constexpr int64_t kSetPlaylistDebounceMs = 500;  // 防抖时间间隔（毫秒）
```

**说明：**
- `last_set_playlist_time_`: 记录上次成功执行 `HandleMusicSetPlaylist` 的时间戳（微秒）
- `kSetPlaylistDebounceMs`: 防抖时间间隔，设置为 500 毫秒

#### 2. 修改 HandleMusicSetPlaylist 函数

在 `application.cc` 的 `HandleMusicSetPlaylist` 函数开头添加防抖检查逻辑：

```cpp
void Application::HandleMusicSetPlaylist(const cJSON* data) {
    // 防抖逻辑：检查是否在短时间内重复调用
    int64_t current_time = esp_timer_get_time();  // 获取当前时间（微秒）
    int64_t time_diff_ms = (current_time - last_set_playlist_time_) / 1000;  // 转换为毫秒

    if (last_set_playlist_time_ > 0 && time_diff_ms < kSetPlaylistDebounceMs) {
        ESP_LOGW(TAG, "HandleMusicSetPlaylist called too frequently (%.0f ms), ignoring to prevent device crash",
                 (double)time_diff_ms);
        return;
    }

    // 更新最后调用时间
    last_set_playlist_time_ = current_time;

    // 原有的播放列表处理逻辑
    // ...
}
```

**工作流程：**
1. 获取当前时间戳（使用 ESP-IDF 的 `esp_timer_get_time()`）
2. 计算与上次调用的时间差
3. 如果时间差小于 500ms，则忽略本次调用并记录警告日志
4. 如果时间差大于等于 500ms，则更新时间戳并继续执行

## 防抖时间选择

选择 **500 毫秒** 作为防抖时间间隔的理由：

1. **用户体验**：500ms 对于音乐切换来说是一个合理的响应时间
2. **设备安全**：足够长的时间间隔可以确保上一个播放列表设置完成
3. **兼容性**：不会影响正常的用户操作（如快速切歌）

如果实际使用中发现：
- **500ms 太短**：可以增加到 1000ms（1秒）
- **500ms 太长**：可以减少到 300ms

## 优势

1. **防止设备崩溃**：有效过滤短时间内的重复指令
2. **轻量级实现**：只需两个成员变量，无需额外的定时器
3. **低开销**：时间检查的性能消耗极小
4. **易于调整**：通过修改 `kSetPlaylistDebounceMs` 常量即可调整防抖时间

## 日志输出

当防抖机制生效时，会输出如下警告日志：

```
W (12345) Application: HandleMusicSetPlaylist called too frequently (123 ms), ignoring to prevent device crash
```

这有助于：
- 调试和监控防抖机制的工作情况
- 识别可能存在的客户端重复发送问题
- 优化防抖时间参数

## 测试建议

1. **正常场景测试**：
   - 间隔 > 500ms 的正常播放列表切换应该正常工作

2. **防抖场景测试**：
   - 快速连续发送多个播放列表指令（间隔 < 500ms）
   - 验证只有第一个指令被执行，后续指令被忽略
   - 检查日志中是否有防抖警告信息

3. **边界测试**：
   - 测试接近 500ms 的时间间隔
   - 验证防抖逻辑的准确性

## 相关文件

- `main/application.h` (第 95-97 行)
- `main/application.cc` (第 964-976 行)

## 修改历史

- 2025-12-06: 添加防抖机制，防止短时间内重复下发 set_playlist 指令导致设备崩溃
