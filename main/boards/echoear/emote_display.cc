#include "emote_display.h"

#include <cstring>
#include <memory>
#include <unordered_map>
#include <tuple>
#include <algorithm>
#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/time.h>
#include <time.h>

#include "display/lcd_display.h"
#include "mmap_generate_assets.h"
#include "config.h"
#include "gfx.h"
#include "decoder/gfx_img_decoder.h"

namespace anim {

static const char* TAG = "emoji";

// AAF animation for fullscreen display

// UI element management
static gfx_obj_t* obj_label_tips = nullptr;
static gfx_obj_t* obj_label_time = nullptr;
static gfx_obj_t* obj_anim_eye = nullptr;
static gfx_obj_t* obj_anim_mic = nullptr;
static gfx_obj_t* obj_img_icon = nullptr;
static gfx_obj_t* obj_img_picture = nullptr;
static gfx_image_dsc_t icon_img_dsc;
static gfx_image_dsc_t picture_img_dsc;

// Track current icon to determine when to show time
static int current_icon_type = MMAP_ASSETS_ICON_BATTERY_BIN;

enum class UIDisplayMode : uint8_t {
    SHOW_ANIM_TOP = 1,  // Show obj_anim_mic
    SHOW_TIME = 2,      // Show obj_label_time
    SHOW_TIPS = 3,      // Show obj_label_tips
    SHOW_PICTURE = 4    // Show obj_img_picture
};

static void SetUIDisplayMode(UIDisplayMode mode)
{
    gfx_obj_set_visible(obj_anim_mic, false);
    gfx_obj_set_visible(obj_label_time, false);
    gfx_obj_set_visible(obj_label_tips, false);
    gfx_obj_set_visible(obj_img_picture, false);

    // Show the selected control
    switch (mode) {
    case UIDisplayMode::SHOW_ANIM_TOP:
        gfx_obj_set_visible(obj_anim_mic, true);
        break;
    case UIDisplayMode::SHOW_TIME:
        gfx_obj_set_visible(obj_label_time, true);
        break;
    case UIDisplayMode::SHOW_TIPS:
        gfx_obj_set_visible(obj_label_tips, true);
        break;
    case UIDisplayMode::SHOW_PICTURE:
        gfx_obj_set_visible(obj_img_picture, true);
        break;
    }
}

static void clock_tm_callback(void* user_data)
{
    // Only display time when battery icon is shown
    if (current_icon_type == MMAP_ASSETS_ICON_BATTERY_BIN) {
        time_t now;
        struct tm timeinfo;
        time(&now);

        setenv("TZ", "GMT+0", 1);
        tzset();
        localtime_r(&now, &timeinfo);

        char time_str[6];
        snprintf(time_str, sizeof(time_str), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

        gfx_label_set_text(obj_label_time, time_str);
        SetUIDisplayMode(UIDisplayMode::SHOW_TIME);
    }
}

static void InitializeAssets(mmap_assets_handle_t* assets_handle)
{
    const mmap_assets_config_t assets_cfg = {
        .partition_label = "assets_A",
        .max_files = MMAP_ASSETS_FILES,
        .checksum = MMAP_ASSETS_CHECKSUM,
        .flags = {.mmap_enable = true, .full_check = true}
    };

    mmap_assets_new(&assets_cfg, assets_handle);
}

static void InitializeGraphics(esp_lcd_panel_handle_t panel, gfx_handle_t* engine_handle)
{
    gfx_core_config_t gfx_cfg = {
        .flush_cb = EmoteEngine::OnFlush,
        .user_data = panel,
        .flags = {
            .swap = true,
            .double_buffer = true,
            .buff_dma = true,
        },
        .h_res = DISPLAY_WIDTH,
        .v_res = DISPLAY_HEIGHT,
        .fps = 30,
        .buffers = {
            .buf1 = nullptr,
            .buf2 = nullptr,
            .buf_pixels = DISPLAY_WIDTH * 16,
        },
        .task = GFX_EMOTE_INIT_CONFIG()
    };

    gfx_cfg.task.task_stack_caps = MALLOC_CAP_DEFAULT;
    gfx_cfg.task.task_affinity = 0;
    gfx_cfg.task.task_priority = 5;
    gfx_cfg.task.task_stack = 20 * 1024;

    *engine_handle = gfx_emote_init(&gfx_cfg);
}

static void InitializeEyeAnimation(gfx_handle_t engine_handle, mmap_assets_handle_t assets_handle)
{
    obj_anim_eye = gfx_anim_create(engine_handle);

    const void* anim_data = mmap_assets_get_mem(assets_handle, MMAP_ASSETS_IDLE_ONE_AAF);
    size_t anim_size = mmap_assets_get_size(assets_handle, MMAP_ASSETS_IDLE_ONE_AAF);

    gfx_anim_set_src(obj_anim_eye, anim_data, anim_size);

    gfx_obj_align(obj_anim_eye, GFX_ALIGN_LEFT_MID, 10, -20);
    gfx_anim_set_mirror(obj_anim_eye, true, (DISPLAY_WIDTH - (173 + 10) * 2));
    gfx_anim_set_segment(obj_anim_eye, 0, 0xFFFF, 20, false);
    gfx_anim_start(obj_anim_eye);
}

static void InitializeFont(gfx_handle_t engine_handle, mmap_assets_handle_t assets_handle)
{
    gfx_font_t font;
    gfx_label_cfg_t font_cfg = {
        .name = "DejaVuSans.ttf",
        .mem = mmap_assets_get_mem(assets_handle, MMAP_ASSETS_KAITI_TTF),
        .mem_size = static_cast<size_t>(mmap_assets_get_size(assets_handle, MMAP_ASSETS_KAITI_TTF)),
    };
    gfx_label_new_font(engine_handle, &font_cfg, &font);

    ESP_LOGI(TAG, "stack: %d", uxTaskGetStackHighWaterMark(nullptr));
}

static void InitializeLabels(gfx_handle_t engine_handle)
{
    // Initialize tips label
    obj_label_tips = gfx_label_create(engine_handle);
    gfx_obj_align(obj_label_tips, GFX_ALIGN_TOP_MID, 0, 45);
    gfx_obj_set_size(obj_label_tips, 160, 40);
    gfx_label_set_text(obj_label_tips, "启动中...");
    gfx_label_set_font_size(obj_label_tips, 20);
    gfx_label_set_color(obj_label_tips, GFX_COLOR_HEX(0xFFFFFF));
    gfx_label_set_text_align(obj_label_tips, GFX_TEXT_ALIGN_LEFT);
    gfx_label_set_long_mode(obj_label_tips, GFX_LABEL_LONG_SCROLL);
    gfx_label_set_scroll_speed(obj_label_tips, 20);
    gfx_label_set_scroll_loop(obj_label_tips, true);

    // Initialize time label
    obj_label_time = gfx_label_create(engine_handle);
    gfx_obj_align(obj_label_time, GFX_ALIGN_TOP_MID, 0, 30);
    gfx_obj_set_size(obj_label_time, 160, 50);
    gfx_label_set_text(obj_label_time, "--:--");
    gfx_label_set_font_size(obj_label_time, 40);
    gfx_label_set_color(obj_label_time, GFX_COLOR_HEX(0xFFFFFF));
    gfx_label_set_text_align(obj_label_time, GFX_TEXT_ALIGN_CENTER);
}

static void InitializeMicAnimation(gfx_handle_t engine_handle, mmap_assets_handle_t assets_handle)
{
    obj_anim_mic = gfx_anim_create(engine_handle);
    gfx_obj_align(obj_anim_mic, GFX_ALIGN_TOP_MID, 0, 25);

    const void* anim_data = mmap_assets_get_mem(assets_handle, MMAP_ASSETS_LISTEN_AAF);
    size_t anim_size = mmap_assets_get_size(assets_handle, MMAP_ASSETS_LISTEN_AAF);
    gfx_anim_set_src(obj_anim_mic, anim_data, anim_size);
    gfx_anim_start(obj_anim_mic);
    gfx_obj_set_visible(obj_anim_mic, false);
}

static void InitializeIcon(gfx_handle_t engine_handle, mmap_assets_handle_t assets_handle)
{
    obj_img_icon = gfx_img_create(engine_handle);
    gfx_obj_align(obj_img_icon, GFX_ALIGN_TOP_MID, -100, 38);

    SetupImageDescriptor(assets_handle, &icon_img_dsc, MMAP_ASSETS_ICON_WIFI_FAILED_BIN);
    gfx_img_set_src(obj_img_icon, static_cast<void*>(&icon_img_dsc));
}

static void InitializePicture(gfx_handle_t engine_handle)
{
    obj_img_picture = gfx_img_create(engine_handle);
    gfx_obj_align(obj_img_picture, GFX_ALIGN_CENTER, 0, 0);
    gfx_obj_set_visible(obj_img_picture, false);
}

static void RegisterCallbacks(esp_lcd_panel_io_handle_t panel_io, gfx_handle_t engine_handle)
{
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = EmoteEngine::OnFlushIoReady,
    };
    esp_lcd_panel_io_register_event_callbacks(panel_io, &cbs, engine_handle);
}

void SetupImageDescriptor(mmap_assets_handle_t assets_handle,
                          gfx_image_dsc_t* img_dsc,
                          int asset_id)
{
    const void* img_data = mmap_assets_get_mem(assets_handle, asset_id);
    size_t img_size = mmap_assets_get_size(assets_handle, asset_id);

    std::memcpy(&img_dsc->header, img_data, sizeof(gfx_image_header_t));
    img_dsc->data = static_cast<const uint8_t*>(img_data) + sizeof(gfx_image_header_t);
    img_dsc->data_size = img_size - sizeof(gfx_image_header_t);
}

EmoteEngine::EmoteEngine(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io)
{
    ESP_LOGI(TAG, "Create EmoteEngine, panel: %p, panel_io: %p", panel, panel_io);

    panel_handle_ = panel;  // Store panel handle for direct LCD access
    InitializeAssets(&assets_handle_);
    InitializeGraphics(panel, &engine_handle_);
    

    gfx_emote_lock(engine_handle_);
    gfx_emote_set_bg_color(engine_handle_, GFX_COLOR_HEX(0x000000));

    // Initialize all UI components
    InitializeEyeAnimation(engine_handle_, assets_handle_);
    InitializeFont(engine_handle_, assets_handle_);
    InitializeLabels(engine_handle_);
    InitializeMicAnimation(engine_handle_, assets_handle_);
    InitializeIcon(engine_handle_, assets_handle_);
    InitializePicture(engine_handle_);  // Required for ShowPicture functionality

    current_icon_type = MMAP_ASSETS_ICON_WIFI_FAILED_BIN;
    SetUIDisplayMode(UIDisplayMode::SHOW_TIPS);

    gfx_timer_create(engine_handle_, clock_tm_callback, 1000, obj_label_tips);

    gfx_emote_unlock(engine_handle_);

    RegisterCallbacks(panel_io, engine_handle_);
}

EmoteEngine::~EmoteEngine()
{
    if (engine_handle_) {
        gfx_emote_deinit(engine_handle_);
        engine_handle_ = nullptr;
    }

    if (assets_handle_) {
        mmap_assets_del(assets_handle_);
        assets_handle_ = nullptr;
    }
}

void EmoteEngine::setEyes(int aaf, bool repeat, int fps)
{
    if (!engine_handle_) {
        return;
    }

    const void* src_data = mmap_assets_get_mem(assets_handle_, aaf);
    size_t src_len = mmap_assets_get_size(assets_handle_, aaf);

    Lock();
    gfx_anim_set_src(obj_anim_eye, src_data, src_len);
    gfx_anim_set_segment(obj_anim_eye, 0, 0xFFFF, fps, repeat);
    gfx_anim_start(obj_anim_eye);
    Unlock();
}

void EmoteEngine::stopEyes()
{
    // Implementation if needed
}

void EmoteEngine::Lock()
{
    if (engine_handle_) {
        gfx_emote_lock(engine_handle_);
    }
}

void EmoteEngine::Unlock()
{
    if (engine_handle_) {
        gfx_emote_unlock(engine_handle_);
    }
}

void EmoteEngine::SetIcon(int asset_id)
{
    if (!engine_handle_) {
        return;
    }

    Lock();
    SetupImageDescriptor(assets_handle_, &icon_img_dsc, asset_id);
    gfx_img_set_src(obj_img_icon, static_cast<void*>(&icon_img_dsc));
    current_icon_type = asset_id;
    Unlock();
}


bool EmoteEngine::OnFlushIoReady(esp_lcd_panel_io_handle_t panel_io,
                                 esp_lcd_panel_io_event_data_t* edata,
                                 void* user_ctx)
{
    return true;
}

void EmoteEngine::OnFlush(gfx_handle_t handle, int x_start, int y_start,
                          int x_end, int y_end, const void* color_data)
{
    auto* panel = static_cast<esp_lcd_panel_handle_t>(gfx_emote_get_user_data(handle));
    if (panel) {
        esp_lcd_panel_draw_bitmap(panel, x_start, y_start, x_end, y_end, color_data);
    }
    gfx_emote_flush_ready(handle, true);
}

// EmoteDisplay implementation
EmoteDisplay::EmoteDisplay(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io)
{
    InitializeEngine(panel, panel_io);
}

EmoteDisplay::~EmoteDisplay() = default;

void EmoteDisplay::SetEmotion(const char* emotion)
{
    if (!engine_) {
        return;
    }

    using EmotionParam = std::tuple<int, bool, int>;
    static const std::unordered_map<std::string, EmotionParam> emotion_map = {
        {"happy",       {MMAP_ASSETS_HAPPY_ONE_AAF,     true,  20}},
        {"laughing",    {MMAP_ASSETS_ENJOY_ONE_AAF,     true,  20}},
        {"funny",       {MMAP_ASSETS_HAPPY_ONE_AAF,     true,  20}},
        {"loving",      {MMAP_ASSETS_HAPPY_ONE_AAF,     true,  20}},
        {"embarrassed", {MMAP_ASSETS_HAPPY_ONE_AAF,     true,  20}},
        {"confident",   {MMAP_ASSETS_HAPPY_ONE_AAF,     true,  20}},
        {"delicious",   {MMAP_ASSETS_HAPPY_ONE_AAF,     true,  20}},
        {"sad",         {MMAP_ASSETS_SAD_ONE_AAF,       true,  20}},
        {"crying",      {MMAP_ASSETS_HAPPY_ONE_AAF,     true,  20}},
        {"sleepy",      {MMAP_ASSETS_HAPPY_ONE_AAF,     true,  20}},
        {"silly",       {MMAP_ASSETS_HAPPY_ONE_AAF,     true,  20}},
        {"angry",       {MMAP_ASSETS_ANGRY_ONE_AAF,     true,  20}},
        {"surprised",   {MMAP_ASSETS_HAPPY_ONE_AAF,     true,  20}},
        {"shocked",     {MMAP_ASSETS_SHOCKED_ONE_AAF,   true,  20}},
        {"thinking",    {MMAP_ASSETS_THINKING_ONE_AAF,  true,  20}},
        {"winking",     {MMAP_ASSETS_HAPPY_ONE_AAF,     true,  20}},
        {"relaxed",     {MMAP_ASSETS_HAPPY_ONE_AAF,     true,  20}},
        {"confused",    {MMAP_ASSETS_DIZZY_ONE_AAF,     true,  20}},
        {"neutral",     {MMAP_ASSETS_IDLE_ONE_AAF,      false, 20}},
        {"idle",        {MMAP_ASSETS_IDLE_ONE_AAF,      false, 20}},
    };

    auto it = emotion_map.find(emotion);
    if (it != emotion_map.end()) {
        int aaf = std::get<0>(it->second);
        bool repeat = std::get<1>(it->second);
        int fps = std::get<2>(it->second);
        engine_->setEyes(aaf, repeat, fps);
    }
}

void EmoteDisplay::SetChatMessage(const char* role, const char* content)
{
    engine_->Lock();
    if (content && strlen(content) > 0) {
        gfx_label_set_text(obj_label_tips, content);
        SetUIDisplayMode(UIDisplayMode::SHOW_TIPS);
    }
    engine_->Unlock();
}

void EmoteDisplay::ShowPicture(int asset_id)
{
    if (!engine_) {
        return;
    }
    
    ESP_LOGI("EmoteDisplay", "ShowPicture called for asset %d (using AAF format)", asset_id);
    
    // First, completely hide all UI elements and pause gfx rendering
    engine_->Lock();
    if (obj_anim_eye) gfx_obj_set_visible(obj_anim_eye, false);
    if (obj_anim_mic) gfx_obj_set_visible(obj_anim_mic, false);
    if (obj_label_time) gfx_obj_set_visible(obj_label_time, false);
    if (obj_label_tips) gfx_obj_set_visible(obj_label_tips, false);
    if (obj_img_icon) gfx_obj_set_visible(obj_img_icon, false);
    if (obj_img_picture) gfx_obj_set_visible(obj_img_picture, false);
    
    
    // Clean up previous fullscreen animation if exists
    if (fullscreen_anim_) {
        gfx_obj_delete(fullscreen_anim_);
        fullscreen_anim_ = nullptr;
    }
    
    // Use AAF animation for full-screen image display
    ESP_LOGI("EmoteDisplay", "Displaying AAF asset %d as full-screen image", asset_id);
    
    // Create a new animation object for full-screen display
    fullscreen_anim_ = gfx_anim_create(engine_->GetGfxHandle());
    if (!fullscreen_anim_) {
        ESP_LOGE("EmoteDisplay", "Failed to create fullscreen animation object");
        engine_->Unlock();
        return;
    }
    
    // Set animation to center and full screen
    gfx_obj_align(fullscreen_anim_, GFX_ALIGN_CENTER, 0, 0);
    
    // Load AAF data
    const void* aaf_data = mmap_assets_get_mem(engine_->GetAssetsHandle(), asset_id);
    size_t aaf_size = mmap_assets_get_size(engine_->GetAssetsHandle(), asset_id);
    
    if (aaf_data && aaf_size > 0) {
        gfx_anim_set_src(fullscreen_anim_, aaf_data, aaf_size);
        gfx_anim_set_segment(fullscreen_anim_, 0, 0xFFFF, 1, false); // Single frame, no repeat
        gfx_obj_set_visible(fullscreen_anim_, true);
        gfx_anim_start(fullscreen_anim_);
        
        ESP_LOGI("EmoteDisplay", "AAF full-screen image started");
    } else {
        ESP_LOGE("EmoteDisplay", "Failed to load AAF asset %d", asset_id);
        gfx_obj_delete(fullscreen_anim_);
        fullscreen_anim_ = nullptr;
    }
    
    engine_->Unlock();
}

void EmoteDisplay::HidePicture()
{
    ESP_LOGI("EmoteDisplay", "Hiding picture and restoring GFX UI");
    
    if (!engine_) {
        return;
    }
    
    engine_->Lock();
    
    // Clean up fullscreen animation
    if (fullscreen_anim_) {
        gfx_obj_delete(fullscreen_anim_);
        fullscreen_anim_ = nullptr;
        ESP_LOGI("EmoteDisplay", "Fullscreen animation cleaned up");
    }
    
    // 隐藏全屏图片 (both AAF animation and .bin image)
    gfx_obj_set_visible(obj_img_picture, false);
    
    // 恢复默认UI状态：显示眼部动画和提示文本
    if (obj_anim_eye) gfx_obj_set_visible(obj_anim_eye, true);
    if (obj_label_time) gfx_obj_set_visible(obj_label_time, true);
    if (obj_label_tips) gfx_obj_set_visible(obj_label_tips, true);
    if (obj_img_icon) gfx_obj_set_visible(obj_img_icon, true);
    SetUIDisplayMode(UIDisplayMode::SHOW_TIPS);
    
    engine_->Unlock();
    
    // Force redraw the GFX UI to clear any direct panel drawing
    // This will restore the normal GFX rendering
    ESP_LOGI("EmoteDisplay", "GFX UI restored");
}

void EmoteDisplay::ShowImageBin(int asset_id)
{
    if (!engine_) {
        return;
    }
    
    ESP_LOGI("EmoteDisplay", "ShowImageBin called for asset %d", asset_id);
    
    // Hide all UI elements first
    engine_->Lock();
    if (obj_anim_eye) gfx_obj_set_visible(obj_anim_eye, false);
    if (obj_anim_mic) gfx_obj_set_visible(obj_anim_mic, false);
    if (obj_label_time) gfx_obj_set_visible(obj_label_time, false);
    if (obj_label_tips) gfx_obj_set_visible(obj_label_tips, false);
    if (obj_img_icon) gfx_obj_set_visible(obj_img_icon, false);
    
    // Clean up previous fullscreen animation
    if (fullscreen_anim_) {
        gfx_obj_delete(fullscreen_anim_);
        fullscreen_anim_ = nullptr;
    }
    
    // Setup .bin image for fullscreen display
    SetupImageDescriptor(engine_->GetAssetsHandle(), &picture_img_dsc, asset_id);
    gfx_img_set_src(obj_img_picture, static_cast<void*>(&picture_img_dsc));
    
    // Position image for fullscreen display (center it)
    gfx_obj_align(obj_img_picture, GFX_ALIGN_CENTER, 0, 0);
    
    // Show the image
    gfx_obj_set_visible(obj_img_picture, true);
    
    engine_->Unlock();
    
    ESP_LOGI("EmoteDisplay", ".bin image displayed fullscreen");
}

void EmoteDisplay::ShowJpegImage(int asset_id)
{
    if (!engine_) {
        return;
    }
    
    ESP_LOGI("EmoteDisplay", "ShowJpegImage called for asset %d", asset_id);
    
    // Hide all UI elements first
    engine_->Lock();
    if (obj_anim_eye) gfx_obj_set_visible(obj_anim_eye, false);
    if (obj_anim_mic) gfx_obj_set_visible(obj_anim_mic, false);
    if (obj_label_time) gfx_obj_set_visible(obj_label_time, false);
    if (obj_label_tips) gfx_obj_set_visible(obj_label_tips, false);
    if (obj_img_icon) gfx_obj_set_visible(obj_img_icon, false);
    
    // Clean up previous fullscreen animation
    if (fullscreen_anim_) {
        gfx_obj_delete(fullscreen_anim_);
        fullscreen_anim_ = nullptr;
    }
    
    // Get JPEG data from assets
    const void* jpeg_data = mmap_assets_get_mem(engine_->GetAssetsHandle(), asset_id);
    size_t jpeg_size = mmap_assets_get_size(engine_->GetAssetsHandle(), asset_id);
    
    if (!jpeg_data || jpeg_size == 0) {
        ESP_LOGE("EmoteDisplay", "Failed to load JPEG asset %d", asset_id);
        engine_->Unlock();
        return;
    }
    
    ESP_LOGI("EmoteDisplay", "ShowJpegImage(asset_id=%d, size=%zu bytes)", asset_id, jpeg_size);
    
    // Validate JPEG format
    const uint8_t* data_bytes = static_cast<const uint8_t*>(jpeg_data);
    if (jpeg_size < 2 || data_bytes[0] != 0xFF || data_bytes[1] != 0xD8) {
        ESP_LOGE("EmoteDisplay", "ShowJpegImage failed: asset_id=%d is not JPEG format", asset_id);
        engine_->Unlock();
        return;
    }
    
    // Setup JPEG descriptor for GFX
    jpeg_img_dsc_.header.magic = 0xFFD8;  // JPEG magic number
    jpeg_img_dsc_.header.cf = GFX_COLOR_FORMAT_RGB565A8;
    jpeg_img_dsc_.header.flags = 0;
    jpeg_img_dsc_.header.w = DISPLAY_WIDTH;   // Will be updated by decoder
    jpeg_img_dsc_.header.h = DISPLAY_HEIGHT;  // Will be updated by decoder
    jpeg_img_dsc_.header.stride = DISPLAY_WIDTH * 2;
    jpeg_img_dsc_.header.reserved = 0;
    jpeg_img_dsc_.data_size = jpeg_size;
    jpeg_img_dsc_.data = static_cast<const uint8_t*>(jpeg_data);
    jpeg_img_dsc_.reserved = nullptr;
    jpeg_img_dsc_.reserved_2 = nullptr;
    
    // Set JPEG source to picture object
    gfx_img_set_src(obj_img_picture, static_cast<void*>(&jpeg_img_dsc_));
    ESP_LOGI("EmoteDisplay", "ShowJpegImage called for 11");
    // Position for fullscreen display
    gfx_obj_align(obj_img_picture, GFX_ALIGN_CENTER, 0, 0);
    
    // Show the JPEG image
    gfx_obj_set_visible(obj_img_picture, true);
    
    engine_->Unlock();
    
    ESP_LOGI("EmoteDisplay", "ShowJpegImage completed: asset_id=%d displayed", asset_id);
}

void EmoteDisplay::SetStatus(const char* status)
{
    if (!engine_) {
        return;
    }

    if (std::strcmp(status, "聆听中...") == 0) {
        SetUIDisplayMode(UIDisplayMode::SHOW_ANIM_TOP);
        engine_->setEyes(MMAP_ASSETS_HAPPY_ONE_AAF, true, 20);
        engine_->SetIcon(MMAP_ASSETS_ICON_MIC_BIN);
    } else if (std::strcmp(status, "待命") == 0) {
        SetUIDisplayMode(UIDisplayMode::SHOW_TIME);
        engine_->SetIcon(MMAP_ASSETS_ICON_BATTERY_BIN);
    } else if (std::strcmp(status, "说话中...") == 0) {
        SetUIDisplayMode(UIDisplayMode::SHOW_TIPS);
        engine_->SetIcon(MMAP_ASSETS_ICON_SPEAKER_ZZZ_BIN);
    } else if (std::strcmp(status, "错误") == 0) {
        SetUIDisplayMode(UIDisplayMode::SHOW_TIPS);
        engine_->SetIcon(MMAP_ASSETS_ICON_WIFI_FAILED_BIN);
    }

    engine_->Lock();
    if (std::strcmp(status, "连接中...") != 0) {
        gfx_label_set_text(obj_label_tips, status);
    }
    engine_->Unlock();
}

void EmoteDisplay::InitializeEngine(esp_lcd_panel_handle_t panel, esp_lcd_panel_io_handle_t panel_io)
{
    engine_ = std::make_unique<EmoteEngine>(panel, panel_io);
}

bool EmoteDisplay::Lock(int timeout_ms)
{
    return true;
}

void EmoteDisplay::Unlock()
{
    // Implementation if needed
}


} // namespace anim
