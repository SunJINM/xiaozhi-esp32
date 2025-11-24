# iOS系统BLE MAC地址获取解决方案

## 📋 问题概述

### 当前挑战
- **Android系统**: 可以直接获取蓝牙设备的MAC地址用于设备识别
- **iOS系统**: 出于隐私保护,CoreBluetooth框架对MAC地址进行不可逆加密,无法直接获取真实MAC地址
- **业务需求**: 需要在iOS和Android平台统一的设备识别方案

### iOS限制原因
根据Apple的设计理念,iOS不暴露BLE设备的MAC地址主要基于以下考虑:
1. **用户隐私保护**: 防止应用和第三方通过MAC地址追踪用户行为
2. **安全性考虑**: 防止MAC地址被用于设备指纹识别和用户画像
3. **防止滥用**: 阻止未授权的设备追踪和定位

## 🔍 研究发现

### iOS系统的替代标识符

#### 1. Peripheral UUID (iOS自动生成)
```
特点:
- iOS为每个BLE设备生成一个128位UUID
- 在同一iOS设备上相对稳定
- 不同iOS设备看到的同一BLE设备UUID不同
- 用户重置网络设置或恢复出厂设置后会改变

限制:
✗ 无法跨iOS设备识别同一BLE设备
✗ 无法与Android平台的MAC地址对应
✗ 不适合作为设备唯一标识符
```

#### 2. Manufacturer Data (厂商自定义数据) ✅ **推荐方案**
```
特点:
+ 可以在BLE广播包中携带自定义数据
+ iOS和Android都能读取
+ 无需建立BLE连接即可获取
+ 可以包含MAC地址、序列号等设备信息

优势:
✓ 跨平台兼容性好
✓ 实现简单直接
✓ 符合BLE标准规范
✓ 无需修改现有配网流程
```

#### 3. Service Data (服务数据)
```
特点:
+ 与特定GATT服务UUID关联
+ 可携带自定义数据
+ iOS和Android都支持

限制:
- 需要定义服务UUID
- 比Manufacturer Data稍复杂
```

#### 4. Device Name (设备名称)
```
当前实现:
deviceName = "XIAOZHI_A1B2C3D4E5F6"  (已包含MAC地址)

特点:
+ 已经在使用
+ iOS和Android都能读取
+ 可以直接从名称解析MAC地址

限制:
- 名称长度限制(建议≤20字符)
- 可能与其他用途冲突
- 不够灵活
```

## ✅ 推荐解决方案

### 方案一: Manufacturer Data (首选) 🏆

#### 实现原理
在BLE广播包的Manufacturer Specific Data字段中添加MAC地址信息。

#### 技术规范
```
Manufacturer Data格式:
┌──────────────────┬────────────────────────┐
│ Company ID (2B)  │ Custom Data (变长)      │
├──────────────────┼────────────────────────┤
│ 0xFF 0xF0        │ MAC Address (6 bytes)  │
└──────────────────┴────────────────────────┘

总长度: 8字节
- Company ID: 0xFFF0 (自定义/测试用)
- MAC Address: 6字节原始MAC地址
```

#### ESP32端实现代码

**1. 修改 wifi_board.cc 添加Manufacturer Data**

```cpp
void WifiBoard::EnterWifiConfigMode() {
    // ... 现有初始化代码 ...

    // 获取蓝牙MAC地址
    uint8_t mac_addr[6];
    esp_err_t mac_ret = esp_read_mac(mac_addr, ESP_MAC_BT);

    if (mac_ret == ESP_OK) {
        // 设置Manufacturer Data
        uint8_t mfg_data[8];
        // Company ID: 0xFFF0 (Little Endian)
        mfg_data[0] = 0xF0;
        mfg_data[1] = 0xFF;
        // MAC Address (6 bytes)
        memcpy(&mfg_data[2], mac_addr, 6);

        // 设置到广播数据中
        esp_ble_adv_data_t adv_data = {0};
        adv_data.set_scan_rsp = false;
        adv_data.include_name = true;
        adv_data.include_txpower = true;
        adv_data.min_interval = 0x0006;
        adv_data.max_interval = 0x0010;
        adv_data.manufacturer_len = sizeof(mfg_data);
        adv_data.p_manufacturer_data = mfg_data;

        // 配置广播数据
        esp_err_t ret = esp_ble_gap_config_adv_data(&adv_data);
        if (ret) {
            ESP_LOGE(TAG, "Config adv data failed: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Manufacturer data set with MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                     mac_addr[0], mac_addr[1], mac_addr[2],
                     mac_addr[3], mac_addr[4], mac_addr[5]);
        }
    }

    // ... 继续现有BluFi初始化 ...
}
```

**2. 在BluFi初始化后配置广播数据**

```cpp
// 在 ESP_BLUFI_EVENT_INIT_FINISH 事件中配置
case ESP_BLUFI_EVENT_INIT_FINISH:
    ESP_LOGI(TAG, "BLUFI init finish");

    // 设置设备名称
    if (!g_device_name.empty()) {
        esp_ble_gap_set_device_name(g_device_name.c_str());
    }

    // 获取并设置Manufacturer Data
    uint8_t mac_addr[6];
    if (esp_read_mac(mac_addr, ESP_MAC_BT) == ESP_OK) {
        uint8_t mfg_data[8];
        mfg_data[0] = 0xF0;  // Company ID低字节
        mfg_data[1] = 0xFF;  // Company ID高字节
        memcpy(&mfg_data[2], mac_addr, 6);

        esp_ble_adv_data_t adv_data = {0};
        adv_data.set_scan_rsp = false;
        adv_data.include_name = true;
        adv_data.include_txpower = false;
        adv_data.manufacturer_len = sizeof(mfg_data);
        adv_data.p_manufacturer_data = mfg_data;

        esp_ble_gap_config_adv_data(&adv_data);
    }

    // 启动广播
    esp_blufi_adv_start();
    break;
```

#### iOS端读取代码 (Swift)

```swift
// 在扫描到设备时的回调函数中
func centralManager(_ central: CBCentralManager,
                   didDiscover peripheral: CBPeripheral,
                   advertisementData: [String : Any],
                   rssi RSSI: NSNumber) {

    // 读取Manufacturer Data
    if let manufacturerData = advertisementData[CBAdvertisementDataManufacturerDataKey] as? Data {

        // 检查长度是否正确 (Company ID 2字节 + MAC 6字节)
        guard manufacturerData.count >= 8 else { return }

        // 提取Company ID
        let companyID = UInt16(manufacturerData[0]) | (UInt16(manufacturerData[1]) << 8)

        // 检查是否是我们的Company ID (0xFFF0)
        if companyID == 0xFFF0 {
            // 提取MAC地址 (bytes 2-7)
            let macBytes = manufacturerData.subdata(in: 2..<8)
            let macAddress = macBytes.map { String(format: "%02X", $0) }.joined(separator: ":")

            print("Device MAC Address: \(macAddress)")
            print("Device Name: \(peripheral.name ?? "Unknown")")

            // 现在可以使用MAC地址进行设备识别
            // 例如: 与服务器验证、本地缓存匹配等
        }
    }
}
```

#### Android端读取代码 (Kotlin/Java)

```kotlin
// Android端可以继续使用原有方式获取MAC
// 但为了统一,也可以读取Manufacturer Data

override fun onScanResult(callbackType: Int, result: ScanResult) {
    val scanRecord = result.scanRecord

    // 方式1: 直接获取MAC地址 (Android特有)
    val macAddress = result.device.address

    // 方式2: 从Manufacturer Data获取 (跨平台方式)
    val manufacturerData = scanRecord?.getManufacturerSpecificData(0xFFF0)
    if (manufacturerData != null && manufacturerData.size == 6) {
        val macFromData = manufacturerData.joinToString(":") {
            String.format("%02X", it)
        }
        println("MAC from Manufacturer Data: $macFromData")
        println("MAC from Device: $macAddress")
        // 两者应该一致
    }
}
```

### 方案二: Service Data (备选)

#### 实现原理
使用Service Data字段携带MAC地址,与特定服务UUID关联。

#### ESP32端实现

```cpp
// 定义自定义服务UUID (128位)
#define XIAOZHI_SERVICE_UUID "0000FF00-0000-1000-8000-00805F9B34FB"

void setup_service_data_advertising() {
    uint8_t mac_addr[6];
    esp_read_mac(mac_addr, ESP_MAC_BT);

    // 配置Service Data
    esp_ble_adv_data_t adv_data = {0};
    adv_data.set_scan_rsp = false;
    adv_data.include_name = true;

    // Service UUID
    static uint8_t service_uuid[16] = {
        0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
        0x00, 0x10, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00
    };

    adv_data.service_uuid_len = 16;
    adv_data.p_service_uuid = service_uuid;

    // Service Data (MAC address)
    adv_data.service_data_len = 6;
    adv_data.p_service_data = mac_addr;

    esp_ble_gap_config_adv_data(&adv_data);
}
```

#### iOS端读取

```swift
func centralManager(_ central: CBCentralManager,
                   didDiscover peripheral: CBPeripheral,
                   advertisementData: [String : Any],
                   rssi RSSI: NSNumber) {

    // 读取Service Data
    if let serviceData = advertisementData[CBAdvertisementDataServiceDataKey] as? [CBUUID: Data] {

        let xiaozhiUUID = CBUUID(string: "0000FF00-0000-1000-8000-00805F9B34FB")

        if let macData = serviceData[xiaozhiUUID] {
            let macAddress = macData.map { String(format: "%02X", $0) }.joined(separator: ":")
            print("MAC from Service Data: \(macAddress)")
        }
    }
}
```

### 方案三: 增强设备名称方案 (最简单)

#### 优化当前实现
当前已在设备名称中包含MAC地址: `XIAOZHI_A1B2C3D4E5F6`

#### iOS端解析

```swift
func centralManager(_ central: CBCentralManager,
                   didDiscover peripheral: CBPeripheral,
                   advertisementData: [String : Any],
                   rssi RSSI: NSNumber) {

    guard let deviceName = peripheral.name else { return }

    // 检查设备名称格式
    if deviceName.hasPrefix("XIAOZHI_") {
        // 提取MAC地址部分
        let macString = String(deviceName.dropFirst(8))  // 去掉 "XIAOZHI_"

        // 格式化为标准MAC地址格式 (添加冒号)
        if macString.count == 12 {
            let macAddress = stride(from: 0, to: 12, by: 2).map {
                let start = macString.index(macString.startIndex, offsetBy: $0)
                let end = macString.index(start, offsetBy: 2)
                return String(macString[start..<end])
            }.joined(separator: ":")

            print("MAC Address: \(macAddress)")
            // 例如: A1:B2:C3:D4:E5:F6
        }
    }
}
```

**优点:**
- 无需修改ESP32代码
- 实现最简单
- 即刻可用

**缺点:**
- 设备名称长度受限
- 不够灵活
- 名称可能被修改

## 📊 方案对比

| 方案 | 难度 | 兼容性 | 灵活性 | 安全性 | 推荐指数 |
|------|------|--------|--------|--------|----------|
| **Manufacturer Data** | ⭐⭐ | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ |
| Service Data | ⭐⭐⭐ | ⭐⭐⭐⭐ | ⭐⭐⭐⭐ | ⭐⭐⭐⭐ | ⭐⭐⭐⭐ |
| 设备名称解析 | ⭐ | ⭐⭐⭐⭐⭐ | ⭐⭐ | ⭐⭐⭐ | ⭐⭐⭐ |

## 🎯 实施建议

### 推荐实施方案: **Manufacturer Data + 设备名称双重保证**

#### 理由:
1. **Manufacturer Data**作为主要方案,提供标准化的跨平台MAC地址传输
2. **设备名称**作为备用方案,确保在Manufacturer Data读取失败时仍可识别设备
3. 两种方案互补,提高系统健壮性

#### 实施步骤:

**阶段1: ESP32端修改 (高优先级)**
```
1. 修改 wifi_board.cc 的 EnterWifiConfigMode() 函数
2. 在BluFi初始化后添加Manufacturer Data配置
3. 测试广播包是否包含正确的Manufacturer Data
4. 验证不影响现有BluFi配网流程
```

**阶段2: iOS App端修改**
```
1. 修改BLE扫描回调函数
2. 添加Manufacturer Data解析逻辑
3. 保留设备名称解析作为fallback
4. 更新设备识别和匹配逻辑
```

**阶段3: Android App端适配**
```
1. 更新为统一使用Manufacturer Data获取MAC
2. 保持向后兼容
3. 统一iOS和Android的设备识别逻辑
```

**阶段4: 测试验证**
```
1. iOS设备BLE扫描测试
2. Android设备BLE扫描测试
3. 跨平台设备识别一致性测试
4. 配网流程完整性测试
```

## 🔧 代码集成示例

### 完整的ESP32实现

```cpp
// wifi_board.cc

// 全局变量
static uint8_t g_mac_addr[6];

void WifiBoard::EnterWifiConfigMode() {
    // ... 现有初始化代码 ...

    // 获取蓝牙MAC地址
    esp_err_t mac_ret = esp_read_mac(g_mac_addr, ESP_MAC_BT);
    if (mac_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get Bluetooth MAC address: %s",
                 esp_err_to_name(mac_ret));
        g_device_name = "XIAOZHI_BLUFI";
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            display->ShowQrCode(g_device_name.c_str());
        }
    } else {
        // 生成设备名称 (保持现有逻辑)
        char mac_str_for_name[13];
        sprintf(mac_str_for_name, "%02X%02X%02X%02X%02X%02X",
                g_mac_addr[0], g_mac_addr[1], g_mac_addr[2],
                g_mac_addr[3], g_mac_addr[4], g_mac_addr[5]);

        g_device_name = "XIAOZHI_" + std::string(mac_str_for_name);
        ESP_LOGI(TAG, "Will set device name to %s", g_device_name.c_str());

        // 生成二维码 (保持现有逻辑)
        char mac_str_for_qr[18];
        sprintf(mac_str_for_qr, "%02X:%02X:%02X:%02X:%02X:%02X",
                g_mac_addr[0], g_mac_addr[1], g_mac_addr[2],
                g_mac_addr[3], g_mac_addr[4], g_mac_addr[5]);

        std::string qr_data = "deviceName=" + g_device_name +
                             "&mac=" + mac_str_for_qr;
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            display->ShowQrCode(qr_data.c_str());
        }
    }

    // ... BluFi初始化代码 ...
}

// 修改blufi_event_callback函数
static void blufi_event_callback(esp_blufi_cb_event_t event,
                                 esp_blufi_cb_param_t *param) {
    ESP_LOGI(TAG, "Blufi event: %d", event);
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        ESP_LOGI(TAG, "BLUFI init finish");

        // 设置设备名称
        if (!g_device_name.empty()) {
            esp_err_t name_ret = esp_ble_gap_set_device_name(g_device_name.c_str());
            if (name_ret) {
                ESP_LOGE(TAG, "Set device name failed: %s",
                        esp_err_to_name(name_ret));
            } else {
                ESP_LOGI(TAG, "Device name set to %s", g_device_name.c_str());
            }
        }

        // 配置Manufacturer Data
        uint8_t mfg_data[8];
        mfg_data[0] = 0xF0;  // Company ID低字节
        mfg_data[1] = 0xFF;  // Company ID高字节
        memcpy(&mfg_data[2], g_mac_addr, 6);  // MAC地址

        esp_ble_adv_data_t adv_data = {0};
        adv_data.set_scan_rsp = false;
        adv_data.include_name = true;
        adv_data.include_txpower = false;
        adv_data.min_interval = 0x0006;
        adv_data.max_interval = 0x0010;
        adv_data.appearance = 0x00;
        adv_data.manufacturer_len = sizeof(mfg_data);
        adv_data.p_manufacturer_data = mfg_data;
        adv_data.flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);

        esp_err_t adv_ret = esp_ble_gap_config_adv_data(&adv_data);
        if (adv_ret) {
            ESP_LOGE(TAG, "Config adv data failed: %s",
                    esp_err_to_name(adv_ret));
        } else {
            ESP_LOGI(TAG, "Manufacturer data configured with MAC: "
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                    g_mac_addr[0], g_mac_addr[1], g_mac_addr[2],
                    g_mac_addr[3], g_mac_addr[4], g_mac_addr[5]);
        }

        // 启动广播
        esp_blufi_adv_start();
        break;

    // ... 其他事件处理保持不变 ...
    }
}
```

### 完整的iOS实现

```swift
import CoreBluetooth

class BLEManager: NSObject, CBCentralManagerDelegate {
    private var centralManager: CBCentralManager!

    override init() {
        super.init()
        centralManager = CBCentralManager(delegate: self, queue: nil)
    }

    // 开始扫描
    func startScanning() {
        let options: [String: Any] = [
            CBCentralManagerScanOptionAllowDuplicatesKey: false
        ]
        centralManager.scanForPeripherals(withServices: nil, options: options)
    }

    // 扫描回调
    func centralManager(_ central: CBCentralManager,
                       didDiscover peripheral: CBPeripheral,
                       advertisementData: [String : Any],
                       rssi RSSI: NSNumber) {

        // 方式1: 从Manufacturer Data获取MAC (推荐)
        if let manufacturerData = advertisementData[CBAdvertisementDataManufacturerDataKey] as? Data {
            if let macAddress = extractMACFromManufacturerData(manufacturerData) {
                print("✅ MAC from Manufacturer Data: \(macAddress)")
                print("   Device Name: \(peripheral.name ?? "Unknown")")
                print("   RSSI: \(RSSI) dBm")

                // 使用MAC地址进行设备识别
                handleDiscoveredDevice(mac: macAddress,
                                      name: peripheral.name,
                                      peripheral: peripheral)
                return
            }
        }

        // 方式2: 从设备名称解析MAC (备用)
        if let deviceName = peripheral.name, deviceName.hasPrefix("XIAOZHI_") {
            if let macAddress = extractMACFromDeviceName(deviceName) {
                print("⚠️ MAC from Device Name (fallback): \(macAddress)")
                print("   Device Name: \(deviceName)")
                print("   RSSI: \(RSSI) dBm")

                // 使用MAC地址进行设备识别
                handleDiscoveredDevice(mac: macAddress,
                                      name: deviceName,
                                      peripheral: peripheral)
            }
        }
    }

    // 从Manufacturer Data提取MAC地址
    private func extractMACFromManufacturerData(_ data: Data) -> String? {
        // 检查数据长度 (至少8字节: 2字节Company ID + 6字节MAC)
        guard data.count >= 8 else {
            return nil
        }

        // 提取Company ID
        let companyID = UInt16(data[0]) | (UInt16(data[1]) << 8)

        // 检查是否是XIAOZHI的Company ID (0xFFF0)
        guard companyID == 0xFFF0 else {
            return nil
        }

        // 提取MAC地址 (bytes 2-7)
        let macBytes = data.subdata(in: 2..<8)
        let macAddress = macBytes.map { String(format: "%02X", $0) }
                                 .joined(separator: ":")

        return macAddress
    }

    // 从设备名称提取MAC地址 (备用方案)
    private func extractMACFromDeviceName(_ name: String) -> String? {
        // 检查格式: XIAOZHI_XXXXXXXXXXXX
        guard name.hasPrefix("XIAOZHI_"), name.count == 20 else {
            return nil
        }

        // 提取MAC字符串
        let macString = String(name.dropFirst(8))

        // 格式化为标准MAC地址格式
        guard macString.count == 12 else {
            return nil
        }

        let macAddress = stride(from: 0, to: 12, by: 2).map {
            let start = macString.index(macString.startIndex, offsetBy: $0)
            let end = macString.index(start, offsetBy: 2)
            return String(macString[start..<end])
        }.joined(separator: ":")

        return macAddress
    }

    // 处理发现的设备
    private func handleDiscoveredDevice(mac: String,
                                       name: String?,
                                       peripheral: CBPeripheral) {
        // 检查是否是目标设备
        if isTargetDevice(mac: mac) {
            print("🎯 Found target device: \(mac)")

            // 停止扫描
            centralManager.stopScan()

            // 连接设备
            centralManager.connect(peripheral, options: nil)
        }
    }

    // 检查是否是目标设备 (根据业务逻辑实现)
    private func isTargetDevice(mac: String) -> Bool {
        // 示例: 检查是否在白名单中
        // 或者: 与用户扫描的二维码中的MAC匹配
        return true
    }

    // 中心管理器状态更新
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            print("✅ Bluetooth is powered on")
            startScanning()
        case .poweredOff:
            print("❌ Bluetooth is powered off")
        case .unauthorized:
            print("⚠️ Bluetooth is unauthorized")
        case .unsupported:
            print("❌ Bluetooth is not supported")
        default:
            print("⏳ Bluetooth state: \(central.state.rawValue)")
        }
    }
}
```

## 📱 iOS App集成清单

### 1. Info.plist配置
```xml
<key>NSBluetoothAlwaysUsageDescription</key>
<string>需要使用蓝牙来配置设备WiFi网络</string>

<key>NSBluetoothPeripheralUsageDescription</key>
<string>需要使用蓝牙来配置设备WiFi网络</string>
```

### 2. 权限请求
```swift
import CoreBluetooth

// 在需要时请求蓝牙权限
func requestBluetoothPermission() {
    let centralManager = CBCentralManager(delegate: self, queue: nil)
    // 初始化会自动触发权限请求
}
```

### 3. 二维码扫描优化
```swift
// 扫描设备二维码后解析
func handleQRCode(content: String) {
    // 解析: deviceName=XIAOZHI_A1B2C3D4E5F6&mac=A1:B2:C3:D4:E5:F6
    let components = content.components(separatedBy: "&")
    var deviceName: String?
    var expectedMAC: String?

    for component in components {
        let keyValue = component.components(separatedBy: "=")
        if keyValue.count == 2 {
            switch keyValue[0] {
            case "deviceName":
                deviceName = keyValue[1]
            case "mac":
                expectedMAC = keyValue[1]
            default:
                break
            }
        }
    }

    // 保存期望的MAC地址,用于设备匹配
    if let mac = expectedMAC {
        UserDefaults.standard.set(mac, forKey: "expectedDeviceMAC")
        print("期望连接的设备MAC: \(mac)")
    }
}
```

## 🔒 安全性考虑

### 1. MAC地址的安全性
- MAC地址本身不是敏感信息,IEEE公开分配
- 在BLE广播中暴露MAC地址是行业标准做法
- Android系统本就允许读取MAC地址

### 2. 防止中间人攻击
```cpp
// ESP32端: 可选的额外验证
// 在Manufacturer Data中添加校验码

uint8_t mfg_data[10];
mfg_data[0] = 0xF0;
mfg_data[1] = 0xFF;
memcpy(&mfg_data[2], mac_addr, 6);

// 简单校验: MAC地址的CRC-16
uint16_t crc = esp_crc16_be(0, mac_addr, 6);
mfg_data[8] = crc & 0xFF;
mfg_data[9] = (crc >> 8) & 0xFF;
```

### 3. 配网过程安全性
- 保持现有的DH密钥交换和AES加密
- Manufacturer Data只用于设备识别,不传输敏感信息
- SSID和密码仍然通过加密通道传输

## 🧪 测试验证计划

### 测试用例1: ESP32广播包验证
```
工具: nRF Connect (Android/iOS)
步骤:
1. ESP32进入配网模式
2. 使用nRF Connect扫描设备
3. 查看Manufacturer Data字段
4. 验证Company ID = 0xFFF0
5. 验证MAC地址字节正确

预期结果:
Manufacturer Data: F0 FF A1 B2 C3 D4 E5 F6
```

### 测试用例2: iOS App MAC提取
```
设备: iPhone (iOS 15+)
步骤:
1. 运行iOS App
2. 扫描XIAOZHI设备
3. 从Manufacturer Data读取MAC
4. 从设备名称读取MAC
5. 对比两种方式获取的MAC

预期结果:
两种方式获取的MAC地址一致
```

### 测试用例3: 跨平台一致性
```
设备: iPhone + Android手机
步骤:
1. 同时扫描同一XIAOZHI设备
2. iOS从Manufacturer Data获取MAC
3. Android从device.address获取MAC
4. Android从Manufacturer Data获取MAC

预期结果:
三种方式获取的MAC地址完全一致
```

### 测试用例4: 配网流程完整性
```
步骤:
1. iOS扫描并识别设备(通过MAC)
2. 建立BLE连接
3. 完成DH密钥协商
4. 发送WiFi凭证
5. 验证配网成功

预期结果:
整个流程正常,MAC识别不影响配网
```

## 📚 参考资料

### BLE规范文档
- [Bluetooth Core Specification](https://www.bluetooth.com/specifications/specs/)
- [Advertising Data Format](https://www.bluetooth.com/specifications/assigned-numbers/generic-access-profile/)

### ESP-IDF文档
- [ESP-BLE-MESH - GAP API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/bluetooth/esp_gap_ble.html)
- [BluFi Protocol](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/blufi.html)

### iOS开发文档
- [Core Bluetooth Programming Guide](https://developer.apple.com/library/archive/documentation/NetworkingInternetWeb/Conceptual/CoreBluetooth_concepts/)
- [CBAdvertisementData Constants](https://developer.apple.com/documentation/corebluetooth/cbadvertisementdata)

### 技术文章
- [iOS BLE MAC Address Accessibility](https://stackoverflow.com/questions/18973098/get-mac-address-of-bluetooth-low-energy-peripheral-in-ios)
- [BLE Advertising Packet Design](https://argenox.com/library/bluetooth-low-energy/designing-ble-advertising-packets/)

## ✅ 实施清单

- [ ] ESP32端代码修改
  - [ ] 在wifi_board.cc添加Manufacturer Data配置
  - [ ] 在blufi_event_callback中处理广播数据设置
  - [ ] 编译测试验证

- [ ] iOS App端代码修改
  - [ ] 添加Manufacturer Data解析功能
  - [ ] 实现MAC地址提取函数
  - [ ] 添加fallback机制(设备名称解析)
  - [ ] 更新设备识别逻辑

- [ ] Android App端代码修改
  - [ ] 统一使用Manufacturer Data获取MAC
  - [ ] 保持向后兼容

- [ ] 测试验证
  - [ ] nRF Connect工具验证广播包
  - [ ] iOS真机测试
  - [ ] Android真机测试
  - [ ] 跨平台一致性测试
  - [ ] 完整配网流程测试

- [ ] 文档更新
  - [ ] 更新技术文档
  - [ ] 更新App开发文档
  - [ ] 添加故障排查指南

## 🎉 预期收益

1. **iOS平台支持**: 完美解决iOS无法获取MAC地址的问题
2. **跨平台统一**: iOS和Android使用统一的设备识别方案
3. **向后兼容**: 不影响现有配网流程和Android功能
4. **符合标准**: 使用BLE标准的Manufacturer Data字段
5. **易于维护**: 代码改动小,逻辑清晰,易于理解和维护

---

**文档版本**: v1.0
**创建日期**: 2025-11-24
**适用产品**: XIAOZHI ESP32系列设备
**作者**: Claude Code AI Assistant
