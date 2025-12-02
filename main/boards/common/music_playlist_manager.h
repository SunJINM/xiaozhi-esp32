#ifndef MUSIC_PLAYLIST_MANAGER_H
#define MUSIC_PLAYLIST_MANAGER_H

#include <vector>
#include <string>
#include <mutex>
#include <functional>

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
    int resource_id;           // 资源ID
    std::string resource_name; // 资源名称
    int duration;              // 时长(秒)

    MusicItem() : item_id(0), resource_id(0), duration(0) {}
    MusicItem(int id, const std::string& url_str, int resource_id, const std::string& name, int dur)
        : item_id(id), url(url_str), resource_id(resource_id), resource_name(name), duration(dur) {}
};

// 播放断点信息结构
struct PlaybackCheckpoint {
    std::string url;           // 播放URL
    int item_id;               // 歌曲ID
    int64_t position_ms;       // 播放位置(毫秒)
    int64_t timestamp_ms;      // 保存时间戳(毫秒)
    bool valid;                // 断点是否有效

    PlaybackCheckpoint()
        : item_id(0), position_ms(0), timestamp_ms(0), valid(false) {}
};

class MusicPlaylistManager {
public:
    MusicPlaylistManager();
    ~MusicPlaylistManager();

    // 播放列表管理
    void SetPlaylist(const std::vector<MusicItem>& playlist);
    void AddToPlaylist(const MusicItem& item);
    void RemoveFromPlaylist(int index);
    void ClearPlaylist();
    const std::vector<MusicItem>& GetPlaylist() const { return playlist_; }
    int GetPlaylistSize() const { return playlist_.size(); }

    // 播放模式和类型
    void SetPlayMode(PlayMode mode);
    void SetPlayType(PlayType type);
    PlayMode GetPlayMode() const { return play_mode_; }
    PlayType GetPlayType() const { return play_type_; }

    // playlist_id 和 resource_type
    void SetPlaylistId(int id) { playlist_id_ = id; }
    void SetResourceType(int type) { resource_type_ = type; }
    int GetPlaylistId() const { return playlist_id_; }
    int GetResourceType() const { return resource_type_; }

    // 当前播放项
    bool SetCurrentByItemId(int item_id);
    bool SetCurrentByIndex(int index);
    int GetCurrentIndex() const { return current_index_; }
    const MusicItem* GetCurrentItem() const;

    // 导航 - 自动播放(受播放模式控制)
    const MusicItem* GetNextItem();     // 获取下一曲(自动播放,受play_mode影响)
    const MusicItem* GetPreviousItem(); // 获取上一曲(自动播放,受play_mode影响)
    bool HasNext() const;               // 是否有下一曲
    bool HasPrevious() const;           // 是否有上一曲

    // 手动控制 - 直接切换(不受播放模式限制)
    const MusicItem* ManualNext();      // 手动切换到下一曲(循环)
    const MusicItem* ManualPrevious();  // 手动切换到上一曲(循环)

    // 播放完成处理
    void OnSongFinished();  // 当前歌曲播放完成回调
    void SetSongFinishedCallback(std::function<void()> callback) {
        on_song_finished_ = callback;
    }

    // 断点管理
    void SaveCheckpoint(const std::string& url, int item_id, int64_t position_ms);
    const PlaybackCheckpoint& GetCheckpoint() const { return checkpoint_; }
    bool HasCheckpoint() const { return checkpoint_.valid; }
    void ClearCheckpoint();

private:
    // 随机播放辅助方法
    const MusicItem* GetNextRandomItem();

    // 数据成员
    int playlist_id_;                          // 歌单ID
    int resource_type_;                        // 资源类型: 1-歌曲, 2-故事
    std::vector<MusicItem> playlist_;          // 播放列表
    PlayMode play_mode_;                       // 播放模式
    PlayType play_type_;                       // 播放类型
    int current_index_;                        // 当前播放索引
    PlaybackCheckpoint checkpoint_;            // 播放断点信息
    mutable std::mutex mutex_;                 // 线程安全 (mutable允许在const方法中使用)
    std::function<void()> on_song_finished_;   // 歌曲完成回调
};

#endif // MUSIC_PLAYLIST_MANAGER_H
