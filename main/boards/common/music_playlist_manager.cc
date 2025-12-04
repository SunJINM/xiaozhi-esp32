#include "music_playlist_manager.h"
#include "system_info.h"
#include <esp_log.h>
#include <esp_random.h>
#include <algorithm>

static const char* TAG = "MusicPlaylistManager";

MusicPlaylistManager::MusicPlaylistManager()
    : playlist_id_(0)
    , resource_type_(0)
    , play_mode_(kPlayModeSequential)
    , play_type_(kPlayTypeLoop)
    , current_index_(-1)
    , on_song_finished_(nullptr) {
}

MusicPlaylistManager::~MusicPlaylistManager() {
}

// ============================================================
// 播放列表管理
// ============================================================

void MusicPlaylistManager::SetPlaylist(const std::vector<MusicItem>& playlist) {
    std::lock_guard<std::mutex> lock(mutex_);
    playlist_ = playlist;
    current_index_ = -1;  // 重置索引
    ESP_LOGI(TAG, "Playlist set with %d items", playlist_.size());
}

void MusicPlaylistManager::AddToPlaylist(const MusicItem& item) {
    std::lock_guard<std::mutex> lock(mutex_);
    playlist_.push_back(item);
    ESP_LOGI(TAG, "Added item to playlist: %s (total: %d)",
             item.resource_name.c_str(), playlist_.size());
}

void MusicPlaylistManager::RemoveFromPlaylist(int index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= 0 && index < playlist_.size()) {
        playlist_.erase(playlist_.begin() + index);
        if (current_index_ >= playlist_.size()) {
            current_index_ = playlist_.size() - 1;
        }
        ESP_LOGI(TAG, "Removed item at index %d (remaining: %d)", index, playlist_.size());
    }
}

void MusicPlaylistManager::ClearPlaylist() {
    std::lock_guard<std::mutex> lock(mutex_);
    playlist_.clear();
    current_index_ = -1;
    ESP_LOGI(TAG, "Playlist cleared");
}

// ============================================================
// 播放模式和类型
// ============================================================

void MusicPlaylistManager::SetPlayMode(PlayMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    play_mode_ = mode;
    ESP_LOGI(TAG, "Play mode set to %d", mode);
}

void MusicPlaylistManager::SetPlayType(PlayType type) {
    std::lock_guard<std::mutex> lock(mutex_);
    play_type_ = type;
    ESP_LOGI(TAG, "Play type set to %d", type);
}

// ============================================================
// 当前播放项
// ============================================================

bool MusicPlaylistManager::SetCurrentByItemId(int item_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < playlist_.size(); i++) {
        if (playlist_[i].item_id == item_id) {
            current_index_ = i;
            ESP_LOGI(TAG, "Current item set to index %d (item_id=%d)", i, item_id);
            return true;
        }
    }
    ESP_LOGW(TAG, "Item with item_id=%d not found in playlist", item_id);
    return false;
}

bool MusicPlaylistManager::SetCurrentByIndex(int index) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= 0 && index < playlist_.size()) {
        current_index_ = index;
        ESP_LOGI(TAG, "Current item set to index %d", index);
        return true;
    }
    ESP_LOGW(TAG, "Invalid index %d (playlist size: %d)", index, playlist_.size());
    return false;
}

const MusicItem* MusicPlaylistManager::GetCurrentItem() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_index_ >= 0 && current_index_ < playlist_.size()) {
        return &playlist_[current_index_];
    }
    return nullptr;
}

// ============================================================
// 导航
// ============================================================

const MusicItem* MusicPlaylistManager::GetNextItem() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (playlist_.empty()) {
        ESP_LOGW(TAG, "GetNextItem: playlist is empty");
        return nullptr;
    }

    // 单次播放模式:播完当前歌曲就停止
    if (play_type_ == kPlayTypeSingle) {
        ESP_LOGI(TAG, "GetNextItem: Single play mode, returning nullptr to stop");
        return nullptr;  // 播完停止,不管play_mode
    }

    // 循环播放模式:根据play_mode决定下一曲
    if (play_mode_ == kPlayModeSequential) {
        // 顺序播放(循环)
        current_index_++;
        if (current_index_ >= playlist_.size()) {
            current_index_ = 0;  // 循环回第一首
        }
        ESP_LOGI(TAG, "GetNextItem: Sequential mode, next index=%d", current_index_);
        return &playlist_[current_index_];

    } else if (play_mode_ == kPlayModeRandom) {
        // 随机播放
        return GetNextRandomItem();

    } else if (play_mode_ == kPlayModeSingleLoop) {
        // 单曲循环:当前索引不变
        ESP_LOGI(TAG, "GetNextItem: Single loop mode, staying at index=%d", current_index_);
        return &playlist_[current_index_];
    }

    return nullptr;
}

const MusicItem* MusicPlaylistManager::GetPreviousItem() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (playlist_.empty()) {
        ESP_LOGW(TAG, "GetPreviousItem: playlist is empty");
        return nullptr;
    }

    // 单次播放模式:没有上一曲
    if (play_type_ == kPlayTypeSingle) {
        ESP_LOGI(TAG, "GetPreviousItem: Single play mode, no previous");
        return nullptr;
    }

    // 循环播放模式
    if (play_mode_ == kPlayModeSequential) {
        // 顺序播放:往前移
        current_index_--;
        if (current_index_ < 0) {
            current_index_ = playlist_.size() - 1;  // 循环到最后一首
        }
        ESP_LOGI(TAG, "GetPreviousItem: Sequential mode, prev index=%d", current_index_);
        return &playlist_[current_index_];

    } else if (play_mode_ == kPlayModeRandom) {
        // 随机播放:随机选一首
        return GetNextRandomItem();

    } else if (play_mode_ == kPlayModeSingleLoop) {
        // 单曲循环:当前索引不变
        ESP_LOGI(TAG, "GetPreviousItem: Single loop mode, staying at index=%d", current_index_);
        return &playlist_[current_index_];
    }

    return nullptr;
}

bool MusicPlaylistManager::HasNext() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (playlist_.empty()) {
        return false;
    }

    // 单次播放模式:没有下一曲
    if (play_type_ == kPlayTypeSingle) {
        return false;
    }

    // 循环播放模式:总是有下一曲
    return true;
}

bool MusicPlaylistManager::HasPrevious() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (playlist_.empty()) {
        return false;
    }

    // 单次播放模式:没有上一曲
    if (play_type_ == kPlayTypeSingle) {
        return false;
    }

    // 循环播放模式:总是有上一曲
    return true;
}

// ============================================================
// 手动控制 - 不受播放模式限制
// ============================================================

const MusicItem* MusicPlaylistManager::ManualNext() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (playlist_.empty()) {
        ESP_LOGW(TAG, "ManualNext: playlist is empty");
        return nullptr;
    }

    // 直接顺序切换,循环到开头
    current_index_++;
    if (current_index_ >= playlist_.size()) {
        current_index_ = 0;
    }

    ESP_LOGI(TAG, "ManualNext: Switched to index=%d", current_index_);
    return &playlist_[current_index_];
}

const MusicItem* MusicPlaylistManager::ManualPrevious() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (playlist_.empty()) {
        ESP_LOGW(TAG, "ManualPrevious: playlist is empty");
        return nullptr;
    }

    // 直接顺序切换,循环到末尾
    current_index_--;
    if (current_index_ < 0) {
        current_index_ = playlist_.size() - 1;
    }

    ESP_LOGI(TAG, "ManualPrevious: Switched to index=%d", current_index_);
    return &playlist_[current_index_];
}

// ============================================================
// 播放完成处理
// ============================================================

void MusicPlaylistManager::OnSongFinished() {
    ESP_LOGI(TAG, "Song finished, getting next item...");

    const MusicItem* next_item = GetNextItem();

    if (next_item) {
        ESP_LOGI(TAG, "Next item found: %s (item_id=%d)",
                 next_item->resource_name.c_str(), next_item->item_id);

        // 调用回调通知外部播放下一曲
        if (on_song_finished_) {
            on_song_finished_();
        }
    } else {
        ESP_LOGI(TAG, "No next item, playlist finished");

        // 调用回调通知播放结束
        if (on_song_finished_) {
            on_song_finished_();
        }
    }
}

// ============================================================
// 私有方法
// ============================================================

const MusicItem* MusicPlaylistManager::GetNextRandomItem() {
    if (playlist_.empty()) {
        return nullptr;
    }

    // 简单随机:从列表中随机选一首
    int random_index = esp_random() % playlist_.size();
    current_index_ = random_index;

    ESP_LOGI(TAG, "GetNextRandomItem: Random selected index=%d", current_index_);
    return &playlist_[current_index_];
}

// ============================================================
// 断点管理
// ============================================================

void MusicPlaylistManager::SaveCheckpoint(const std::string& url, int item_id, int64_t position_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    checkpoint_.url = url;
    checkpoint_.item_id = item_id;
    checkpoint_.position_ms = position_ms;
    checkpoint_.byte_offset = 0;  // 不使用字节偏移（方案A）
    checkpoint_.sample_rate = 0;
    checkpoint_.channels = 0;
    checkpoint_.timestamp_ms = esp_timer_get_time() / 1000;  // 当前时间(毫秒)
    checkpoint_.valid = true;

    ESP_LOGI(TAG, "Checkpoint saved (basic): item_id=%d, position=%lld ms",
             item_id, (long long)position_ms);
}

void MusicPlaylistManager::SaveCheckpointWithFrameInfo(const std::string& url, int item_id,
                                                       int64_t position_ms, size_t byte_offset,
                                                       int sample_rate, int channels) {
    std::lock_guard<std::mutex> lock(mutex_);
    checkpoint_.url = url;
    checkpoint_.item_id = item_id;
    checkpoint_.position_ms = position_ms;
    checkpoint_.byte_offset = byte_offset;
    checkpoint_.sample_rate = sample_rate;
    checkpoint_.channels = channels;
    checkpoint_.timestamp_ms = esp_timer_get_time() / 1000;  // 当前时间(毫秒)
    checkpoint_.valid = true;

    ESP_LOGI(TAG, "Checkpoint saved (full): item_id=%d, position=%lld ms, byte_offset=%zu, rate=%d, ch=%d",
             item_id, (long long)position_ms, byte_offset, sample_rate, channels);
}

void MusicPlaylistManager::ClearCheckpoint() {
    std::lock_guard<std::mutex> lock(mutex_);
    checkpoint_.valid = false;
    checkpoint_.url.clear();
    checkpoint_.item_id = 0;
    checkpoint_.position_ms = 0;
    checkpoint_.byte_offset = 0;
    checkpoint_.sample_rate = 0;
    checkpoint_.channels = 0;
    checkpoint_.timestamp_ms = 0;

    ESP_LOGI(TAG, "Checkpoint cleared");
}
