#include "wifi_board.h"
#include "esp_wifi.h"
#include "esp_netif.h"

#include "display.h"
#include "application.h"
#include "system_info.h"
#include "font_awesome_symbols.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_http.h>
#include <esp_mqtt.h>
#include <esp_udp.h>
#include <tcp_transport.h>
#include <tls_transport.h>
#include <web_socket.h>
#include <esp_log.h>

#include <wifi_station.h>
#include "blufi.h"
#include "esp_blufi_api.h"
#include "esp_blufi.h"
#include "esp_bt.h"
#include "esp_log.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "nvs_flash.h"
#include <ssid_manager.h>
#include "esp_mac.h"

// BluFi related global variables
static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;
const int FAILED_BIT = BIT1;
static bool ble_is_connected = false;
static wifi_config_t sta_config;

// Forward declaration for Blufi callback
static void blufi_event_callback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param);

static esp_blufi_callbacks_t blufi_callbacks = {
    .event_cb = blufi_event_callback,
    .negotiate_data_handler = blufi_dh_negotiate_data_handler,
    .encrypt_func = blufi_aes_encrypt,
    .decrypt_func = blufi_aes_decrypt,
    .checksum_func = blufi_crc_checksum,
};


static const char *TAG = "WifiBoard";




static void ip_event_handler(void* arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data) {
if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
ESP_LOGI(TAG, "Got IP address, setting CONNECTED_BIT");
xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
}
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
int32_t event_id, void* event_data)
{
switch (event_id) {
case WIFI_EVENT_STA_DISCONNECTED:
ESP_LOGI(TAG, "WIFI_EVENT_STA_DISCONNECTED");
xEventGroupSetBits(wifi_event_group, FAILED_BIT);
break;
default:
break;
}
return;
}


WifiBoard::WifiBoard() {
    Settings settings("wifi", true);
    wifi_config_mode_ = settings.GetInt("force_ap") == 1;
    if (wifi_config_mode_) {
        ESP_LOGI(TAG, "force_ap is set to 1, reset to 0");
        settings.SetInt("force_ap", 0);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

static void blufi_event_callback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param) {
    ESP_LOGI(TAG, "Blufi event: %d", event);
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        ESP_LOGI(TAG, "BLUFI init finish");
        esp_blufi_adv_start();
        break;
    case ESP_BLUFI_EVENT_DEINIT_FINISH:
        ESP_LOGI(TAG, "BLUFI deinit finish");
        break;
    case ESP_BLUFI_EVENT_BLE_CONNECT:
        ESP_LOGI(TAG, "BLUFI ble connect");
        ble_is_connected = true;
        esp_blufi_adv_stop();
        blufi_security_init();
        break;
    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        ESP_LOGI(TAG, "BLUFI ble disconnect");
        ble_is_connected = false;
        blufi_security_deinit();
        esp_blufi_adv_start();
        break;
    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        ESP_LOGI(TAG, "BLUFI Set WIFI opmode %d", param->wifi_mode.op_mode);
        ESP_ERROR_CHECK(esp_wifi_set_mode(param->wifi_mode.op_mode));
        break;
    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
        ESP_LOGI(TAG, "BLUFI request wifi connect to AP");
        esp_wifi_disconnect();
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        esp_wifi_connect();
        break;
    case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
        ESP_LOGI(TAG, "BLUFI request wifi disconnect from AP");
        esp_wifi_disconnect();
        break;
    case ESP_BLUFI_EVENT_REPORT_ERROR:
        ESP_LOGE(TAG, "BLUFI report error, error code %d", param->report_error.state);
        esp_blufi_send_error_info(param->report_error.state);
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_STATUS: {
        wifi_mode_t mode;
        esp_wifi_get_mode(&mode);
        // Simplified status reporting, adapt from blufi_example_main.c if needed
        esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_SUCCESS, 0, NULL);
        break;
    }
    case ESP_BLUFI_EVENT_RECV_STA_SSID:
        strncpy((char *)sta_config.sta.ssid, (char *)param->sta_ssid.ssid, param->sta_ssid.ssid_len);
        sta_config.sta.ssid[param->sta_ssid.ssid_len] = '\0';
        ESP_LOGI(TAG, "Recv STA SSID %s", sta_config.sta.ssid);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        strncpy((char *)sta_config.sta.password, (char *)param->sta_passwd.passwd, param->sta_passwd.passwd_len);
        sta_config.sta.password[param->sta_passwd.passwd_len] = '\0';
        ESP_LOGI(TAG, "Recv STA PASSWORD"); // Do not log password
        break;
    // Add other cases from blufi_example_main.c as needed
    default:
        break;
    }
}

void WifiBoard::EnterWifiConfigMode() {
    // 检查可用内存
    size_t free_heap = esp_get_free_heap_size();
    ESP_LOGI(TAG, "Free heap before BluFi init: %d", free_heap);
    
    if (free_heap < 100000) {  // 如果可用内存少于100KB
        ESP_LOGE(TAG, "Insufficient memory for BluFi initialization");
        return;
    }
    auto& application = Application::GetInstance();
    application.SetDeviceState(kDeviceStateWifiConfiguring);

    ESP_LOGI(TAG, "Starting BluFi WiFi configuration");

    // Initialize NVS (Needed for WiFi and BluFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    wifi_event_group = xEventGroupCreate();
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    // esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap(); // Not needed for BluFi STA config
    // assert(ap_netif);

    // Register Wi-Fi event handlers (simplified, adapt from blufi_example_main.c if needed)
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg)); 
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Initialize Bluetooth controller
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "%s initialize bt controller failed: %s", __func__, esp_err_to_name(ret));
        return;
    }
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(TAG, "%s enable bt controller failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    // Use the unified BluFi host initialization function
    ret = esp_blufi_host_and_cb_init(&blufi_callbacks);
    if (ret) {
        ESP_LOGE(TAG, "%s BluFi host and callback init failed: %s", __func__, esp_err_to_name(ret));
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return;
    }

    // Get Bluetooth MAC address
    uint8_t mac_addr[6];
    esp_err_t mac_ret = esp_read_mac(mac_addr, ESP_MAC_BT);
    if (mac_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get Bluetooth MAC address: %s", esp_err_to_name(mac_ret));
        // Fallback to default name if MAC address cannot be read
        const char *device_name = "XIAOZHI_BLUFI";
        esp_err_t name_ret = esp_ble_gap_set_device_name(device_name);
        if (name_ret) {
            ESP_LOGE(TAG, "Set device name failed: %s", esp_err_to_name(name_ret));
        } else {
            ESP_LOGI(TAG, "Device name set to %s", device_name);
        }
        // QR code will only show default name if MAC is unavailable
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            display->ShowQrCode(device_name);
        }
    } else {
        char mac_str_for_name[13]; // 6 bytes * 2 chars + 1 null terminator
        sprintf(mac_str_for_name, "%02X%02X%02X%02X%02X%02X",
                mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        
        std::string device_name_str = "XIAOZHI_" + std::string(mac_str_for_name);
        esp_err_t name_ret = esp_ble_gap_set_device_name(device_name_str.c_str());
        if (name_ret) {
            ESP_LOGE(TAG, "Set device name failed: %s", esp_err_to_name(name_ret));
        } else {
            ESP_LOGI(TAG, "Device name set to %s", device_name_str.c_str());
        }

        char mac_str_for_qr[18];
        sprintf(mac_str_for_qr, "%02X:%02X:%02X:%02X:%02X:%02X",
                mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        ESP_LOGI(TAG, "Bluetooth MAC address: %s", mac_str_for_qr);

        // Combine device name and MAC address for QR code
        std::string qr_data = "deviceName=" + device_name_str + "&mac=" + mac_str_for_qr;
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            display->ShowQrCode(qr_data.c_str());
        }
    }

    free_heap = esp_get_free_heap_size();
    ESP_LOGI(TAG, "Free heap after BluFi init: %d", free_heap);

    ESP_LOGI(TAG, "BLUFI VERSION %04x", esp_blufi_get_version());

    // std::string hint = Lang::Strings::CONNECT_VIA_BLUETOOTH; // Add a new string for Bluetooth
    // application.Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "", Lang::Sounds::P3_WIFICONFIG); // Temporarily commented out voice prompt
    
    // Wait forever until reset after configuration
    while(true) {
        EventBits_t bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT | FAILED_BIT, 
            pdTRUE,
            pdFALSE,
            portMAX_DELAY);
    
        if (bits & CONNECTED_BIT) {
            ESP_LOGI(TAG, "BluFi configuration successful, Wi-Fi connected.");
    
            auto display = Board::GetInstance().GetDisplay();
            if (display) {
                display->ClearQrCode();
            }
            auto& ssid_manager = SsidManager::GetInstance();
            ssid_manager.AddSsid(reinterpret_cast<const char*>(sta_config.sta.ssid), reinterpret_cast<const char*>(sta_config.sta.password));

            // Send success custom data
            cJSON *root_success = cJSON_CreateObject();
            cJSON_AddNumberToObject(root_success, "type", 4);
            cJSON_AddTrueToObject(root_success, "result");
            cJSON *data_success = cJSON_CreateObject();
            cJSON_AddNumberToObject(data_success, "progress", 100);
            cJSON_AddStringToObject(data_success, "ssid", reinterpret_cast<const char*>(sta_config.sta.ssid));
            cJSON_AddItemToObject(root_success, "data", data_success);
            char *json_str_success = cJSON_PrintUnformatted(root_success);
            if (json_str_success) {
                esp_blufi_send_custom_data((uint8_t*)json_str_success, strlen(json_str_success));
                free(json_str_success);
            }
            cJSON_Delete(root_success);
            
            // 反初始化BluFi和蓝牙
            esp_blufi_host_deinit();
            esp_bt_controller_disable();
            esp_bt_controller_deinit();
            break;
        } else if (bits & FAILED_BIT) {
            ESP_LOGE(TAG, "BluFi configuration timed out or failed.");
            // Send failure custom data
            const char *json_str_fail = "wifi connect fail";
            esp_blufi_send_custom_data((uint8_t*)json_str_fail, strlen(json_str_fail));
        }
    }
}

void WifiBoard::StartNetwork() {
    wifi_config_mode_ = true;
    // User can press BOOT button while starting to enter WiFi configuration mode
    if (wifi_config_mode_) {
        EnterWifiConfigMode();
        return;
    }

    // If no WiFi SSID is configured, enter WiFi configuration mode
    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();
    if (ssid_list.empty()) {
        wifi_config_mode_ = true;
        EnterWifiConfigMode();
        return;
    }

    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.OnScanBegin([this]() {
        auto display = Board::GetInstance().GetDisplay();
        display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
    });
    wifi_station.OnConnect([this](const std::string& ssid) {
        auto display = Board::GetInstance().GetDisplay();
        std::string notification = Lang::Strings::CONNECT_TO;
        notification += ssid;
        notification += "...";
        display->ShowNotification(notification.c_str(), 30000);
    });
    wifi_station.OnConnected([this](const std::string& ssid) {
        auto display = Board::GetInstance().GetDisplay();
        std::string notification = Lang::Strings::CONNECTED_TO;
        notification += ssid;
        display->ShowNotification(notification.c_str(), 30000);
    });
    wifi_station.Start();

    // Try to connect to WiFi, if failed, launch the WiFi configuration AP
    if (!wifi_station.WaitForConnected(60 * 1000)) {
        wifi_station.Stop();
        wifi_config_mode_ = true;
        EnterWifiConfigMode();
        return;
    }
}

Http* WifiBoard::CreateHttp() {
    return new EspHttp();
}

WebSocket* WifiBoard::CreateWebSocket() {
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    if (url.find("wss://") == 0) {
        return new WebSocket(new TlsTransport());
    } else {
        return new WebSocket(new TcpTransport());
    }
    return nullptr;
}

Mqtt* WifiBoard::CreateMqtt() {
    return new EspMqtt();
}

Udp* WifiBoard::CreateUdp() {
    return new EspUdp();
}

const char* WifiBoard::GetNetworkStateIcon() {
    if (wifi_config_mode_) {
        return FONT_AWESOME_WIFI;
    }
    auto& wifi_station = WifiStation::GetInstance();
    if (!wifi_station.IsConnected()) {
        return FONT_AWESOME_WIFI_OFF;
    }
    int8_t rssi = wifi_station.GetRssi();
    if (rssi >= -60) {
        return FONT_AWESOME_WIFI;
    } else if (rssi >= -70) {
        return FONT_AWESOME_WIFI_FAIR;
    } else {
        return FONT_AWESOME_WIFI_WEAK;
    }
}

std::string WifiBoard::GetBoardJson() {
    // Set the board type for OTA
    auto& wifi_station = WifiStation::GetInstance();
    std::string board_json = std::string("{\"type\":\"" BOARD_TYPE "\",");
    board_json += "\"name\":\"" BOARD_NAME "\",";
    if (!wifi_config_mode_) {
        board_json += "\"ssid\":\"" + wifi_station.GetSsid() + "\",";
        board_json += "\"rssi\":" + std::to_string(wifi_station.GetRssi()) + ",";
        board_json += "\"channel\":" + std::to_string(wifi_station.GetChannel()) + ",";
        board_json += "\"ip\":\"" + wifi_station.GetIpAddress() + "\",";
    }
    board_json += "\"mac\":\"" + SystemInfo::GetMacAddress() + "\"}";
    return board_json;
}

void WifiBoard::SetPowerSaveMode(bool enabled) {
    auto& wifi_station = WifiStation::GetInstance();
    wifi_station.SetPowerSaveMode(enabled);
}

void WifiBoard::ResetWifiConfiguration() {
    // Set a flag and reboot the device to enter the network configuration mode
    {
        Settings settings("wifi", true);
        settings.SetInt("force_ap", 1);
    }
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);
    vTaskDelay(pdMS_TO_TICKS(1000));
    // Reboot the device
    esp_restart();
}

std::string WifiBoard::GetDeviceStatusJson() {
    /*
     * 返回设备状态JSON
     * 
     * 返回的JSON结构如下：
     * {
     *     "audio_speaker": {
     *         "volume": 70
     *     },
     *     "screen": {
     *         "brightness": 100,
     *         "theme": "light"
     *     },
     *     "battery": {
     *         "level": 50,
     *         "charging": true
     *     },
     *     "network": {
     *         "type": "wifi",
     *         "ssid": "Xiaozhi",
     *         "rssi": -60
     *     },
     *     "chip": {
     *         "temperature": 25
     *     }
     * }
     */
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    auto audio_codec = board.GetAudioCodec();
    if (audio_codec) {
        cJSON_AddNumberToObject(audio_speaker, "volume", audio_codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen brightness
    auto backlight = board.GetBacklight();
    auto screen = cJSON_CreateObject();
    if (backlight) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    auto display = board.GetDisplay();
    if (display && display->height() > 64) { // For LCD display only
        cJSON_AddStringToObject(screen, "theme", display->GetTheme().c_str());
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        cJSON* battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", battery_level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto network = cJSON_CreateObject();
    auto& wifi_station = WifiStation::GetInstance();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi_station.GetSsid().c_str());
    int rssi = wifi_station.GetRssi();
    if (rssi >= -60) {
        cJSON_AddStringToObject(network, "signal", "strong");
    } else if (rssi >= -70) {
        cJSON_AddStringToObject(network, "signal", "medium");
    } else {
        cJSON_AddStringToObject(network, "signal", "weak");
    }
    cJSON_AddItemToObject(root, "network", network);

    // Chip
    float esp32temp = 0.0f;
    if (board.GetTemperature(esp32temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", esp32temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto json_str = cJSON_PrintUnformatted(root);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return json;
}
