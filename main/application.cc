#include "application.h"
#include "board.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "boards/common/music_playlist_manager.h"
#include "settings.h"

#include <cstring>
#include <esp_log.h>
#include <esp_http_client.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>
#include <wifi_station.h>
#define TAG "Application"


static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"
};

Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);

    // 初始化音乐播放管理器
    music_playlist_manager_ = std::make_unique<MusicPlaylistManager>();
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    if (music_status_timer_ != nullptr) {
        esp_timer_stop(music_status_timer_);
        esp_timer_delete(music_status_timer_);
    }
    vEventGroupDelete(event_group_);
}

void Application::CheckNewVersion(Ota& ota) {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒

    auto& board = Board::GetInstance();
    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        if (!ota.CheckVersion()) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, ota.GetCheckVersionUrl().c_str());
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (device_state_ == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // 每次重试后延迟时间翻倍
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // 重置重试延迟时间

        if (ota.HasNewVersion()) {
            Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);

            vTaskDelay(pdMS_TO_TICKS(3000));

            SetDeviceState(kDeviceStateUpgrading);
            
            std::string message = std::string(Lang::Strings::NEW_VERSION) + ota.GetFirmwareVersion();
            display->SetChatMessage("system", message.c_str());

            board.SetPowerSaveMode(false);
            audio_service_.Stop();
            vTaskDelay(pdMS_TO_TICKS(1000));

            bool upgrade_success = ota.StartUpgrade([display](int progress, size_t speed) {
                std::thread([display, progress, speed]() {
                    char buffer[32];
                    snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                    display->SetChatMessage("system", buffer);
                }).detach();
            });

            if (!upgrade_success) {
                // Upgrade failed, restart audio service and continue running
                ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
                audio_service_.Start(); // Restart audio service
                board.SetPowerSaveMode(true); // Restore power save mode
                Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
                vTaskDelay(pdMS_TO_TICKS(3000));
                // Continue to normal operation (don't break, just fall through)
            } else {
                // Upgrade success, reboot immediately
                ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
                display->SetChatMessage("system", "Upgrade successful, rebooting...");
                vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
                Reboot();
                return; // This line will never be reached after reboot
            }
        }

        // No new version, mark the current version as valid
        ota.MarkCurrentVersionValid();
        if (!ota.HasActivationCode() && !ota.HasActivationChallenge()) {
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota.HasActivationCode()) {
            ShowActivationCode(ota.GetActivationCode(), ota.GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota.Activate();
            if (err == ESP_OK) {
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (device_state_ == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::ToggleChatState() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
                SendDeviceStatus();
            }

            SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            auto music = Board::GetInstance().GetMusic();
            if (music->IsPlaying()) {
                HandleMusicControl("pause");
                is_music_playing_ = false;
                music_is_stopped_ = true;
                StopMusicStatusTimer();
                SendMusicStatus(true);
            }
        });
    } else if (device_state_ == kDeviceStateListening) {
        // Schedule([this]() {
        //     protocol_->CloseAudioChannel();
        // });
    }
}

void Application::StartListening() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    if (device_state_ == kDeviceStateIdle) {
        Schedule([this]() {
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    return;
                }
            }

            SetListeningMode(kListeningModeManualStop);
        });
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
            SetListeningMode(kListeningModeManualStop);
        });
    }
}

void Application::StopListening() {
    if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }

    Schedule([this]() {
        if (device_state_ == kDeviceStateListening) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::Start() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    /* Setup the display */
    auto display = board.GetDisplay();

    /* Setup the audio service */
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    audio_service_.SetCallbacks(callbacks);

    /* Setup music callbacks */
    auto music = board.GetMusic();
    if (music != nullptr) {
        music->SetSongFinishedCallback([this]() {
            Schedule([this]() {
                OnMusicSongFinished();
            });
        });
        music->SetErrorCallback([this](const std::string& error) {
            ESP_LOGE(TAG, "Music playback error: %s", error.c_str());
            Schedule([this]() {
                StopMusicStatusTimer();
                SendMusicStatus(true);  // 发送错误状态
            });
        });
    }

    /* Setup charging status callback */
    board.OnChargingStatusChanged([this](bool is_charging) {
        OnChargingStatusChanged(is_charging);
    });

    /* Start the clock timer to update the status bar */
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    /* Wait for the network to be ready */
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);

    // Check for new firmware version or get the MQTT broker address
    Ota ota;
    CheckNewVersion(ota);

    // Initialize the protocol
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    // Add MCP common tools before initializing the protocol
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();

    if (ota.HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota.HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        ESP_LOGI(TAG, "receive audio, is_music_playing is %d", (int)is_music_playing_);
        if (device_state_ == kDeviceStateSpeaking && !is_music_playing_) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveMode(false);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveMode(true);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    protocol_->OnDisconnected([this]() {
        Schedule([this]() {
            ESP_LOGI(TAG, "WebSocket disconnected, stopping music status timer");
            StopMusicStatusTimer();
        });
    });
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (device_state_ == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else if (strcmp(type->valuestring, "music") == 0) {
            ESP_LOGI(TAG, "Received music control");
            auto data = cJSON_GetObjectItem(root, "data");
            if (cJSON_IsObject(data)) {
                Schedule([this, data_str = std::string(cJSON_PrintUnformatted(data))]() {
                    cJSON* data_obj = cJSON_Parse(data_str.c_str());
                    if (data_obj != nullptr) {
                        HandleMusicCommand(data_obj);
                        cJSON_Delete(data_obj);
                    }
                });
            } else {
                ESP_LOGW(TAG, "Invalid music message format: missing data");
            }
        } else if (strcmp(type->valuestring, "bind") == 0) {
            // 响应ping消息，返回pong
            ESP_LOGI(TAG, "Received bind");
            {
                Settings settings("wifi", true);
                settings.SetInt("force_ap", 1);
                settings.SetInt("bind", 1);
            }
            Reboot();
        } else if (strcmp(type->valuestring, "device_status") == 0) {
            // 返回设备状态信息（电量、音量、网络）
            ESP_LOGI(TAG, "Received device_status request");
            Schedule([this]() {
                SendDeviceStatus();
            });
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    bool protocol_started = protocol_->Start();

    // Print heap stats
    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota.HasServerTime();
    if (protocol_started) {
        std::string message = std::string(Lang::Strings::VERSION) + ota.GetCurrentVersion();
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
        display->SetEmotion("happy");
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);

        // vTaskDelay(pdMS_TO_TICKS(500));
        // audio_service_.PlaySound(Lang::Sounds::OGG_BIRTHDAY);
        ToggleChatState();
    }
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    // Raise the priority of the main event loop to avoid being interrupted by background tasks (which has priority 2)
    vTaskPrioritySet(NULL, 3);

    // 记录主事件循环运行的CPU核心
    ESP_LOGI(TAG, "Main event loop running on Core %d with priority 3", xPortGetCoreID());

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE |
            MAIN_EVENT_SEND_AUDIO |
            MAIN_EVENT_WAKE_WORD_DETECTED |
            MAIN_EVENT_VAD_CHANGE |
            MAIN_EVENT_CLOCK_TICK |
            MAIN_EVENT_ERROR, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (!protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            OnWakeWordDetected();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (device_state_ == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print the debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
                // SystemInfo::PrintTaskList();
                SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::OnWakeWordDetected() {
    if (!protocol_) {
        return;
    }

    if (device_state_ == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            if (!protocol_->OpenAudioChannel()) {
                audio_service_.EnableWakeWordDetection(true);
                return;
            }
        }

        auto wake_word = audio_service_.GetLastWakeWord();
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_USE_AFE_WAKE_WORD || CONFIG_USE_CUSTOM_WAKE_WORD
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
#else
        SetListeningMode(aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime);
        // Play the pop up sound to indicate the wake word is detected
        audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
#endif
    } else if (device_state_ == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
    } else if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    protocol_->SendAbortSpeaking(reason);
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        return;
    }
    
    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);

    // Send the state change event
    DeviceStateEventManager::GetInstance().PostStateChangeEvent(previous_state, state);

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            display->SetEmotion("neutral");
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (!audio_service_.IsAudioProcessorRunning()) {
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
                audio_service_.EnableWakeWordDetection(false);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
#if CONFIG_USE_AFE_WAKE_WORD
                audio_service_.EnableWakeWordDetection(true);
#else
                audio_service_.EnableWakeWordDetection(false);
#endif
            }
            audio_service_.ResetDecoder();
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    esp_restart();
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (device_state_ == kDeviceStateIdle) {
        ToggleChatState();
        Schedule([this, wake_word]() {
            if (protocol_) {
                protocol_->SendWakeWordDetected(wake_word); 
            }
        }); 
    } else if (device_state_ == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (device_state_ == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (device_state_ != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    Schedule([this, payload]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

// 新增：接收外部音频数据（如音乐播放）
void Application::AddAudioData(AudioStreamPacket&& packet) {
    auto codec = Board::GetInstance().GetAudioCodec();
    if (device_state_ == kDeviceStateSpeaking && codec->output_enabled()) {
        // packet.payload包含的是原始PCM数据（int16_t）
        if (packet.payload.size() >= 2) {
            size_t num_samples = packet.payload.size() / sizeof(int16_t);
            std::vector<int16_t> pcm_data(num_samples);
            memcpy(pcm_data.data(), packet.payload.data(), packet.payload.size());
            
            // 检查采样率是否匹配，如果不匹配则进行重采样（不再动态切换硬件采样率）
            if (packet.sample_rate != codec->output_sample_rate()) {

                // 验证采样率参数
                if (packet.sample_rate <= 0 || codec->output_sample_rate() <= 0) {
                    ESP_LOGE(TAG, "Invalid sample rates: %d -> %d",
                            packet.sample_rate, codec->output_sample_rate());
                    return;
                }

                std::vector<int16_t> resampled;
                float resample_ratio = codec->output_sample_rate() / static_cast<float>(packet.sample_rate);

                if (resample_ratio > 1.0f) {
                    // 上采样：线性插值
                    size_t expected_size = static_cast<size_t>(pcm_data.size() * resample_ratio + 0.5f);
                    resampled.reserve(expected_size);

                    for (size_t i = 0; i < pcm_data.size(); ++i) {
                        // 添加原始样本
                        resampled.push_back(pcm_data[i]);

                        // 计算需要插值的样本数
                        int interpolation_count = static_cast<int>(resample_ratio) - 1;
                        if (interpolation_count > 0 && i + 1 < pcm_data.size()) {
                            int16_t current = pcm_data[i];
                            int16_t next = pcm_data[i + 1];
                            for (int j = 1; j <= interpolation_count; ++j) {
                                float t = static_cast<float>(j) / (interpolation_count + 1);
                                int16_t interpolated = static_cast<int16_t>(current + (next - current) * t);
                                resampled.push_back(interpolated);
                            }
                        } else if (interpolation_count > 0) {
                            // 最后一个样本，直接重复
                            for (int j = 1; j <= interpolation_count; ++j) {
                                resampled.push_back(pcm_data[i]);
                            }
                        }
                    }
                } else {
                    // 下采样：简单抽取
                    float downsample_step = 1.0f / resample_ratio;
                    for (float pos = 0.0f; pos < pcm_data.size(); pos += downsample_step) {
                        size_t index = static_cast<size_t>(pos);
                        if (index < pcm_data.size()) {
                            resampled.push_back(pcm_data[index]);
                        }
                    }
                }

                pcm_data = std::move(resampled);
            }
            
            // 确保音频输出已启用
            if (!codec->output_enabled()) {
                codec->EnableOutput(true);
            }
            
            // 发送PCM数据到音频编解码器
            codec->OutputData(pcm_data);
            
            audio_service_.UpdateOutputTimestamp();
        }
    }
}

// ============================================================
// 音乐播放控制
// ============================================================

void Application::HandleMusicCommand(const cJSON* data) {
    auto action = cJSON_GetObjectItem(data, "action");
    if (!cJSON_IsString(action)) {
        ESP_LOGW(TAG, "Music command missing action field");
        return;
    }

    std::string action_str = action->valuestring;
    ESP_LOGI(TAG, "Music command: %s", action_str.c_str());

    if (action_str == "set_playlist") {
        HandleMusicSetPlaylist(data);
    } else if (action_str == "play" || action_str == "pause" || action_str == "resume" ||
               action_str == "stop" || action_str == "next" || action_str == "prev") {
        HandleMusicControl(action_str);
    } else if (action_str == "set_mode") {
        HandleMusicSetMode(data);
    } else {
        ESP_LOGW(TAG, "Unknown music action: %s", action_str.c_str());
    }
}

void Application::HandleMusicSetPlaylist(const cJSON* data) {
    auto& board = Board::GetInstance();
    auto music = board.GetMusic();

    // 解析精简指令
    auto playlist_id = cJSON_GetObjectItem(data, "playlist_id");
    auto resource_type = cJSON_GetObjectItem(data, "resource_type");
    auto start_item_id = cJSON_GetObjectItem(data, "start_item_id");
    auto play_mode = cJSON_GetObjectItem(data, "play_mode");
    auto play_type = cJSON_GetObjectItem(data, "play_type");
    auto start_item_url = cJSON_GetObjectItem(data, "start_item_url");
    auto playlist_url = cJSON_GetObjectItem(data, "playlist_url");

    if (!cJSON_IsNumber(playlist_id) || !cJSON_IsNumber(resource_type) ||
        !cJSON_IsNumber(start_item_id) || !cJSON_IsNumber(play_mode) ||
        !cJSON_IsNumber(play_type) || !cJSON_IsString(start_item_url)) {
        ESP_LOGW(TAG, "Invalid set_playlist parameters");
        return;
    }

    int new_playlist_id = playlist_id->valueint;

    // 设置播放列表基本信息
    music_playlist_manager_->SetPlaylistId(new_playlist_id);
    music_playlist_manager_->SetResourceType(resource_type->valueint);
    music_playlist_manager_->SetPlayMode(static_cast<PlayMode>(play_mode->valueint));
    music_playlist_manager_->SetPlayType(static_cast<PlayType>(play_type->valueint));

    ESP_LOGI(TAG, "Playlist set: id=%d, starting first song", new_playlist_id);

    // 平滑切换播放(防止杂音)
    if (music != nullptr) {
        if (!is_music_playing_) {
            AbortSpeaking(kAbortReasonNone);
        }

        is_switching_song_ = true;

        // 淡出当前音乐
        auto codec = board.GetAudioCodec();
        int original_volume = 0;
        if (codec && is_music_playing_) {
            original_volume = codec->output_volume();
            if (original_volume > 0) {
                // 快速淡出(约80ms)
                for (int vol = original_volume; vol >= 0; vol -= 10) {
                    codec->SetOutputVolume(vol);
                    vTaskDelay(pdMS_TO_TICKS(16));
                }
            }
            // 淡出后等待音频播放完毕(StartStreaming会清空缓冲)
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        // 启动新歌
        if (music->StartStreaming(start_item_url->valuestring)) {
            is_music_playing_ = true;
            music_is_stopped_ = false;
            music->SetAutomated(false);
            StartMusicStatusTimer();
            SendMusicStatus(true);

            // 等待新歌缓冲建立后淡入
            vTaskDelay(pdMS_TO_TICKS(100));
            if (codec && original_volume > 0) {
                // 渐进淡入
                for (int vol = 0; vol <= original_volume; vol += 10) {
                    codec->SetOutputVolume(vol);
                    vTaskDelay(pdMS_TO_TICKS(16));
                }
                codec->SetOutputVolume(original_volume);
            }

            ESP_LOGI(TAG, "First song started with smooth transition");
        } else {
            // 启动失败,恢复音量
            if (codec && original_volume > 0) {
                codec->SetOutputVolume(original_volume);
            }
            ESP_LOGE(TAG, "Failed to start first song");
        }
    }

    // 异步拉取完整歌单
    if (cJSON_IsString(playlist_url)) {
        FetchPlaylistAsync(playlist_url->valuestring, new_playlist_id);
    }
}

void Application::HandleMusicControl(const std::string& action) {
    auto& board = Board::GetInstance();
    auto music = board.GetMusic();
    if (music == nullptr) {
        ESP_LOGW(TAG, "Music not available");
        return;
    }

    ESP_LOGI(TAG, "Music control: %s", action.c_str());

    if (action == "play") {
        is_music_playing_ = true;
        music_is_stopped_ = false;  // 开始播放，清除停止标记
        music->PlaySong();
        SendMusicStatus(true);
    } else if (action == "pause") {
        AbortSpeaking(kAbortReasonNone);
        is_music_playing_ = false;
        // 停止播放（完全停止，线程退出）
        music->StopSong();
        music->PauseSong();
        music_is_stopped_ = true;
        SendMusicStatus(true);
        StopMusicStatusTimer();
        // 切换到 listening 状态，等待用户输入
        Schedule([this]() {
            SetDeviceState(kDeviceStateListening);
        });
        // 保存断点信息（方案B：包含字节偏移和帧信息）
        const MusicItem* current_item = music_playlist_manager_->GetCurrentItem();
        if (current_item != nullptr) {
            int64_t position_ms = static_cast<int64_t>(music->GetCurrentPositionMilliseconds());
            size_t byte_offset = music->GetDownloadedBytes();
            int sample_rate = music->GetCurrentSampleRate();
            int channels = music->GetCurrentChannels();

            music_playlist_manager_->SaveCheckpointWithFrameInfo(
                current_item->url,
                current_item->item_id,
                position_ms,
                byte_offset,
                sample_rate,
                channels
            );
            ESP_LOGI(TAG, "Checkpoint saved: item_id=%d, position=%lld ms, byte_offset=%zu, rate=%d, ch=%d",
                     current_item->item_id, (long long)position_ms, byte_offset, sample_rate, channels);
        }
    } else if (action == "resume") {
        // 从断点恢复播放
        if (music_playlist_manager_->HasCheckpoint()) {
            is_music_playing_ = true;
            AbortSpeaking(kAbortReasonNone);
            music_is_stopped_ = false;
            const auto& checkpoint = music_playlist_manager_->GetCheckpoint();
            ESP_LOGI(TAG, "Resuming from checkpoint: item_id=%d, position=%lld ms, byte_offset=%zu",
                     checkpoint.item_id, (long long)checkpoint.position_ms, checkpoint.byte_offset);

            bool resume_success = false;
            music->ResumeSong();
            SendMusicStatus(true);
            StartMusicStatusTimer();

            if (checkpoint.byte_offset > 0 && checkpoint.sample_rate > 0 && checkpoint.channels > 0) {
                ESP_LOGI(TAG, "Using Method B: HTTP Range from byte %zu (rate=%d, ch=%d)",
                         checkpoint.byte_offset, checkpoint.sample_rate, checkpoint.channels);
                resume_success = music->StartStreamingFromByteOffset(checkpoint.url, checkpoint.byte_offset);
            }

            if (resume_success) {

                // 清除断点
                music_playlist_manager_->ClearCheckpoint();
            } else {
                ESP_LOGE(TAG, "Failed to resume from checkpoint");
            }
        }
    } else if (action == "stop") {
        is_music_playing_ = false;
        is_switching_song_ = true;  // 设置切歌标志，防止状态切换到聆听
        music->StopSong();
        music_is_stopped_ = true;  // 设置停止标记
        StopMusicStatusTimer();
        SendMusicStatus(true);  // 发送一次停止状态后，后续不再发送
        ESP_LOGI(TAG, "Music stopped, is_switching_song set to true");
    } else if (action == "next") {
        is_music_playing_ = true;
        // 手动切换下一曲 - 不受播放模式限制
        const MusicItem* next_item = music_playlist_manager_->ManualNext();
        if (next_item != nullptr) {
            ESP_LOGI(TAG, "Manual next: %s (item_id=%d)",
                     next_item->resource_name.c_str(), next_item->item_id);
            if (music->StartStreaming(next_item->url)) {
                music_is_stopped_ = false;  // 切歌后清除停止标记
                SendMusicStatus(true);
            }
        } else {
            ESP_LOGW(TAG, "Manual next failed: playlist empty");
        }
    } else if (action == "prev") {
        is_music_playing_ = true;
        // 手动切换上一曲 - 不受播放模式限制
        const MusicItem* prev_item = music_playlist_manager_->ManualPrevious();
        if (prev_item != nullptr) {
            ESP_LOGI(TAG, "Manual previous: %s (item_id=%d)",
                     prev_item->resource_name.c_str(), prev_item->item_id);
            if (music->StartStreaming(prev_item->url)) {
                music_is_stopped_ = false;  // 切歌后清除停止标记
                SendMusicStatus(true);
            }
        } else {
            ESP_LOGW(TAG, "Manual previous failed: playlist empty");
        }
    }
}

void Application::HandleMusicSetMode(const cJSON* data) {
    auto play_mode = cJSON_GetObjectItem(data, "play_mode");
    auto play_type = cJSON_GetObjectItem(data, "play_type");

    if (cJSON_IsNumber(play_mode)) {
        music_playlist_manager_->SetPlayMode(static_cast<PlayMode>(play_mode->valueint));
        ESP_LOGI(TAG, "Play mode changed to: %d", play_mode->valueint);
    }

    if (cJSON_IsNumber(play_type)) {
        music_playlist_manager_->SetPlayType(static_cast<PlayType>(play_type->valueint));
        ESP_LOGI(TAG, "Play type changed to: %d", play_type->valueint);
    }

    SendMusicStatus(true);
}

void Application::OnMusicSongFinished() {
    ESP_LOGI(TAG, "Song finished callback");

    const MusicItem* next_item = music_playlist_manager_->GetNextItem();

    if (next_item != nullptr) {
        ESP_LOGI(TAG, "Auto-playing next: %s (item_id=%d)",
                 next_item->resource_name.c_str(), next_item->item_id);

        auto& board = Board::GetInstance();
        auto music = board.GetMusic();
        if (music != nullptr) {
            is_music_playing_ = true;
            music->SetAutomated(true);
            if (music->StartStreaming(next_item->url)) {
                music_is_stopped_ = false;  // 自动播放下一曲，清除停止标记
                SendMusicStatus(true);  // 发送新歌曲开始状态
            } else {
                ESP_LOGE(TAG, "Failed to start next item");
                is_music_playing_ = false;
                music_is_stopped_ = true;  // 播放失败，设置停止标记
                StopMusicStatusTimer();
                SendMusicStatus(true);
            }
        }
    } else {
        ESP_LOGI(TAG, "No more items, playlist finished");
        is_music_playing_ = false;
        music_is_stopped_ = true;  // 播放列表结束，设置停止标记
        StopMusicStatusTimer();
        SendMusicStatus(true);  // 发送播放结束状态
    }
}

void Application::SendMusicStatus(bool force) {
    auto& board = Board::GetInstance();
    auto music = board.GetMusic();
    if (music == nullptr) {
        return;
    }

    // 如果音乐已停止且不是强制发送，则不再发送状态消息
    if (music_is_stopped_ && !force) {
        return;
    }

    const MusicItem* current_item = music_playlist_manager_->GetCurrentItem();

    // 构建状态JSON
    cJSON* status = cJSON_CreateObject();
    cJSON_AddNumberToObject(status, "playlist_id", music_playlist_manager_->GetPlaylistId());
    cJSON_AddNumberToObject(status, "resource_type", music_playlist_manager_->GetResourceType());

    if (current_item != nullptr) {
        cJSON_AddNumberToObject(status, "item_id", current_item->item_id);
        cJSON_AddNumberToObject(status, "resource_id", current_item->resource_id);
        cJSON_AddStringToObject(status, "resource_name", current_item->resource_name.c_str());
        cJSON_AddNumberToObject(status, "duration", current_item->duration);
    } else {
        cJSON_AddNumberToObject(status, "item_id", 0);
        cJSON_AddNumberToObject(status, "resource_id", 0);
        cJSON_AddStringToObject(status, "resource_name", "");
        cJSON_AddNumberToObject(status, "duration", 0);
    }

    // 播放状态: 0=停止, 1=播放中, 2=暂停
    // 修复逻辑：暂停时不应该设置为播放中
    int play_status = 0;
    if (music->IsPaused()) {
        play_status = 2;  // 暂停优先判断
    } else if (music->IsPlaying()) {
        play_status = 1;  // 播放中
    }
    cJSON_AddNumberToObject(status, "play_status", play_status);

    // 添加当前播放位置（秒）
    cJSON_AddNumberToObject(status, "position", music->GetCurrentPositionSeconds());

    cJSON_AddNumberToObject(status, "play_mode", music_playlist_manager_->GetPlayMode());
    cJSON_AddNumberToObject(status, "play_type", music_playlist_manager_->GetPlayType());

    char* status_str = cJSON_PrintUnformatted(status);
    if (status_str != nullptr) {
        ESP_LOGI(TAG, "Music status: %s", status_str);

        // 构建完整消息
        cJSON* message = cJSON_CreateObject();
        cJSON_AddStringToObject(message, "type", "music_status");
        cJSON_AddItemToObject(message, "data", status);  // status的所有权转移

        char* message_str = cJSON_PrintUnformatted(message);
        if (message_str != nullptr) {
            protocol_->SendJson(message_str);
            free(message_str);
        }

        cJSON_Delete(message);
        free(status_str);
    } else {
        cJSON_Delete(status);
    }
}

void Application::StartMusicStatusTimer() {
    if (music_status_timer_ != nullptr) {
        return;  // 定时器已存在
    }

    esp_timer_create_args_t timer_args = {
        .callback = MusicStatusTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "music_status_timer",
        .skip_unhandled_events = true
    };

    esp_err_t err = esp_timer_create(&timer_args, &music_status_timer_);
    if (err == ESP_OK) {
        esp_timer_start_periodic(music_status_timer_, 30000000);  // 3秒
        ESP_LOGI(TAG, "Music status timer started");
    } else {
        ESP_LOGE(TAG, "Failed to create music status timer: %d", err);
    }
}

void Application::StopMusicStatusTimer() {
    if (music_status_timer_ != nullptr) {
        esp_timer_stop(music_status_timer_);
        esp_timer_delete(music_status_timer_);
        music_status_timer_ = nullptr;
        ESP_LOGI(TAG, "Music status timer stopped");
    }
}

void Application::MusicStatusTimerCallback(void* arg) {
    Application* app = static_cast<Application*>(arg);
    app->Schedule([app]() {
        app->SendMusicStatus(false);
    });
}

void Application::SendDeviceStatus() {
    auto& board = Board::GetInstance();
    cJSON* status = cJSON_CreateObject();

    // 音量信息
    auto audio_codec = board.GetAudioCodec();
    if (audio_codec) {
        cJSON_AddNumberToObject(status, "volume", audio_codec->output_volume());
    }

    // 电量信息
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        cJSON* battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", battery_level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(status, "battery", battery);
    }

    // 网络信息
    auto& wifi_station = WifiStation::GetInstance();
    cJSON* network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi_station.GetSsid().c_str());
    cJSON_AddNumberToObject(network, "rssi", wifi_station.GetRssi());
    cJSON_AddItemToObject(status, "network", network);

    // 构建完整消息
    cJSON* message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "type", "device_status");
    cJSON_AddItemToObject(message, "data", status);

    char* message_str = cJSON_PrintUnformatted(message);
    if (message_str != nullptr) {
        ESP_LOGI(TAG, "Sending device status: %s", message_str);
        protocol_->SendJson(message_str);
        free(message_str);
    }
    cJSON_Delete(message);
}

void Application::OnChargingStatusChanged(bool is_charging) {
    ESP_LOGI(TAG, "Charging status changed: %s", is_charging ? "charging" : "not charging");
    Schedule([this]() {
        SendDeviceStatus();
    });
}

// HTTPS异步拉取歌单
void Application::FetchPlaylistAsync(const std::string& url, int playlist_id) {
    // 如果playlist_id与当前正在播放的相同,跳过重复拉取
    if (current_playlist_id_ == playlist_id) {
        ESP_LOGI(TAG, "Playlist id=%d already loaded, skip fetching", playlist_id);
        return;
    }

    if (playlist_fetch_task_ != nullptr) {
        vTaskDelete(playlist_fetch_task_);
        playlist_fetch_task_ = nullptr;
    }

    auto params = new FetchParams{this, url, playlist_id};
    xTaskCreate(PlaylistFetchTask, "playlist_fetch", 8192, params, 2, &playlist_fetch_task_);
}

void Application::PlaylistFetchTask(void* arg) {
    auto params = static_cast<FetchParams*>(arg);
    Application* app = params->app;
    std::string url = params->url;
    int playlist_id = params->playlist_id;
    delete params;

    ESP_LOGI(TAG, "Fetching playlist: %s (id=%d)", url.c_str(), playlist_id);

    // HTTPS拉取
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.timeout_ms = 10000;
    config.skip_cert_common_name_check = true;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.crt_bundle_attach = nullptr;

    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        app->playlist_fetch_task_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);

    ESP_LOGI(TAG, "HTTP Status: %d, Content-Length: %d", status_code, content_length);

    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed with status code: %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        app->playlist_fetch_task_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    if (content_length <= 0) {
        ESP_LOGI(TAG, "Content-Length not available or zero, will read until connection closes");
    }

    std::string response_data;
    if (content_length > 0) {
        response_data.reserve(content_length);
    }

    char buffer[1024];
    int read_len;
    while ((read_len = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
        response_data.append(buffer, read_len);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    // 打印前500个字符，避免日志过长
    if (response_data.size() > 500) {
        ESP_LOGI(TAG, "Response content (first 500 chars): %.500s...", response_data.c_str());
    } else {
        ESP_LOGI(TAG, "Response content: %s", response_data.c_str());
    }

    // 解析JSON（服务器直接返回数组）
    cJSON* root = cJSON_Parse(response_data.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "JSON parse failed");
        app->playlist_fetch_task_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // 检查root是否是数组
    if (!cJSON_IsArray(root)) {
        ESP_LOGE(TAG, "Invalid JSON: expected array");
        cJSON_Delete(root);
        app->playlist_fetch_task_ = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    std::vector<MusicItem> playlist;
    int array_size = cJSON_GetArraySize(root);
    ESP_LOGI(TAG, "Parsing %d items from playlist", array_size);

    for (int i = 0; i < array_size; i++) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        // 使用驼峰命名的字段（itemId, resourceId, resourceName）
        auto item_id = cJSON_GetObjectItem(item, "itemId");
        auto url_obj = cJSON_GetObjectItem(item, "url");
        auto resource_id = cJSON_GetObjectItem(item, "resourceId");
        auto resource_name = cJSON_GetObjectItem(item, "resourceName");
        auto duration = cJSON_GetObjectItem(item, "duration");

        if (cJSON_IsNumber(item_id) && cJSON_IsString(url_obj) &&
            cJSON_IsString(resource_name) && cJSON_IsNumber(duration)) {
            playlist.emplace_back(
                item_id->valueint,
                url_obj->valuestring,
                resource_id ? resource_id->valueint : 0,
                resource_name->valuestring,
                duration->valueint
            );
        }
    }

    cJSON_Delete(root);

    // 更新歌单并标记当前playlist_id
    app->Schedule([app, playlist = std::move(playlist), playlist_id]() {
        app->music_playlist_manager_->SetPlaylist(playlist);
        app->current_playlist_id_ = playlist_id; // 拉取成功后才更新ID
        ESP_LOGI(TAG, "Playlist updated with %d items (id=%d)", playlist.size(), playlist_id);
    });

    app->playlist_fetch_task_ = nullptr;
    vTaskDelete(nullptr);
}
