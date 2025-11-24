# iOS BLE MAC地址获取 - 实施总结

## ✅ 已完成的实现

### 问题
- iOS系统无法获取BLE设备的真实MAC地址（隐私保护机制）
- BluFi配网时设备名称固定为 `BLUFI_DEVICE`，无法从设备名解析MAC
- 需要跨平台统一的设备识别方案

### 解决方案
通过在BLE广播包中添加 **Manufacturer Data** 字段来携带设备MAC地址。

### 关键修复：初始化顺序问题
**问题**: 原代码中 `g_device_name` 在 `ESP_BLUFI_EVENT_INIT_FINISH` 事件触发后才被设置，导致自定义的 `esp_blufi_adv_start()` 函数中 `g_device_name` 为空，使用了默认名称 "BLUFI_DEVICE"。

**解决方案**:
1. 将MAC地址读取代码移至 `esp_blufi_host_and_cb_init()` **之前**执行
2. 将 `g_device_name` 准备代码移至 `esp_blufi_host_and_cb_init()` **之前**执行
3. 确保事件触发时 `g_device_name` 已包含正确的 "XIAOZHI_XXXXXXXXXXXX" 值

**代码顺序**:
```cpp
// 1. 初始化蓝牙控制器
esp_bt_controller_enable(ESP_BT_MODE_BLE);

// 2. 读取MAC地址并准备设备名称（关键！必须在BluFi初始化之前）
uint8_t mac_addr[6];
esp_read_mac(mac_addr, ESP_MAC_BT);
g_device_name = "XIAOZHI_" + mac_string;

// 3. NOW 初始化 BluFi（此时g_device_name已就绪）
esp_blufi_host_and_cb_init(&blufi_callbacks);
// → 触发 ESP_BLUFI_EVENT_INIT_FINISH
//   → 调用 esp_blufi_adv_start(g_device_name.c_str())
//   → g_device_name 有值！✅
```

## 📝 代码修改

### 文件: `main/boards/common/wifi_board.cc`

#### 1. 添加全局变量 (第39-41行)
```cpp
// Manufacturer Data for iOS MAC address accessibility
static uint8_t g_manufacturer_data[8];
static bool adv_data_ready = false;
```

#### 2. 重写 `esp_blufi_adv_start()` 函数 (第131-181行)
这是核心实现，每次BluFi启动广播时都会：
1. 设置设备名称
2. 读取蓝牙MAC地址
3. 配置Manufacturer Data
4. 启动BLE广播

```cpp
void esp_blufi_adv_start(const char *name)
{
    // 设置设备名称
    esp_ble_gap_set_device_name(name ?: BLUFI_DEVICE_NAME);

    // 配置Manufacturer Data
    // Company ID: 0xFFF0
    // MAC: 6 bytes
    g_manufacturer_data[0] = 0xF0;
    g_manufacturer_data[1] = 0xFF;
    memcpy(&g_manufacturer_data[2], mac_addr, 6);

    // 配置并启动广播
    esp_ble_gap_config_adv_data(&adv_data);
}
```

#### 3. 修改 `blufi_event_callback()` (第186-195行)
在 `ESP_BLUFI_EVENT_INIT_FINISH` 事件中调用自定义的 `esp_blufi_adv_start()`：
```cpp
case ESP_BLUFI_EVENT_INIT_FINISH:
    // 设置设备名称
    esp_ble_gap_set_device_name(g_device_name.c_str());

    // 启动广播（会自动配置Manufacturer Data）
    esp_blufi_adv_start(g_device_name.c_str());
    break;
```

## 📊 数据格式

### Manufacturer Data 结构
```
总长度: 8 字节

┌──────────┬──────────┬────────────────────────────┐
│  Byte 0  │  Byte 1  │  Bytes 2-7                 │
├──────────┼──────────┼────────────────────────────┤
│   0xF0   │   0xFF   │  MAC Address (6 bytes)     │
│          │          │  例: A1 B2 C3 D4 E5 F6     │
└──────────┴──────────┴────────────────────────────┘

Company ID: 0xFFF0 (Little Endian格式)
用途: 自定义/测试用 Company ID
```

### 实际广播包示例
```
BLE Advertising Packet:
- Device Name: BLUFI_DEVICE
- Manufacturer Data: F0 FF A1 B2 C3 D4 E5 F6
  ├─ Company ID: F0 FF (0xFFF0)
  └─ MAC Address: A1:B2:C3:D4:E5:F6
```

## 🧪 测试验证

### 预期日志输出
```
I (xxx) WifiBoard: Starting BluFi WiFi configuration
I (xxx) WifiBoard: BLUFI init finish
I (xxx) WifiBoard: Device name set to XIAOZHI_A1B2C3D4E5F6
I (xxx) WifiBoard: BLE device name set to: XIAOZHI_A1B2C3D4E5F6
I (xxx) WifiBoard: 📱 Manufacturer Data configured:
I (xxx) WifiBoard:    Company ID: 0xFFF0
I (xxx) WifiBoard:    MAC Address: A1:B2:C3:D4:E5:F6
I (xxx) WifiBoard:    iOS devices can now read MAC from advertising packet
```

### 使用 nRF Connect 验证

#### iOS/Android步骤:
1. 下载安装 **nRF Connect** App
2. 打开App，点击 **SCAN** 开始扫描
3. 找到 `BLUFI_DEVICE` 设备
4. 点击设备查看详情
5. 切换到 **RAW** 或 **Advertising data** 标签
6. 查找 **Manufacturer data** 字段

#### 预期结果:
```
Complete Local Name: BLUFI_DEVICE
Manufacturer data (8 bytes):
  F0 FF A1 B2 C3 D4 E5 F6

解析:
  Company ID: 0xFFF0 (bytes 0-1, Little Endian)
  MAC Address: A1:B2:C3:D4:E5:F6 (bytes 2-7)
```

## 📱 移动端集成指南

### iOS (Swift)
```swift
func centralManager(_ central: CBCentralManager,
                   didDiscover peripheral: CBPeripheral,
                   advertisementData: [String : Any],
                   rssi RSSI: NSNumber) {

    // 读取 Manufacturer Data
    if let mfgData = advertisementData[CBAdvertisementDataManufacturerDataKey] as? Data {
        guard mfgData.count >= 8 else { return }

        // 检查 Company ID (0xFFF0)
        let companyID = UInt16(mfgData[0]) | (UInt16(mfgData[1]) << 8)
        guard companyID == 0xFFF0 else { return }

        // 提取 MAC 地址
        let macBytes = mfgData.subdata(in: 2..<8)
        let macAddress = macBytes.map { String(format: "%02X", $0) }
                                 .joined(separator: ":")

        print("设备MAC: \(macAddress)")
        // 使用MAC进行设备识别和匹配
    }
}
```

### Android (Kotlin)
```kotlin
override fun onScanResult(callbackType: Int, result: ScanResult) {
    val scanRecord = result.scanRecord ?: return

    // 从 Manufacturer Data 读取MAC（跨平台统一方式）
    val mfgData = scanRecord.getManufacturerSpecificData(0xFFF0)
    if (mfgData != null && mfgData.size == 6) {
        val macAddress = mfgData.joinToString(":") {
            String.format("%02X", it)
        }
        println("设备MAC: $macAddress")
        // 使用MAC进行设备识别
    }

    // 或者继续使用Android原生方式（向后兼容）
    val macFromDevice = result.device.address
}
```

## ✅ 实施检查清单

- [x] ESP32端代码实现
  - [x] 添加全局变量存储Manufacturer Data
  - [x] 重写 `esp_blufi_adv_start()` 函数
  - [x] 修改BluFi事件回调
  - [x] 添加详细日志输出

- [ ] 编译和烧录测试
  - [ ] `idf.py build` 编译成功
  - [ ] `idf.py flash` 烧录成功
  - [ ] `idf.py monitor` 查看日志

- [ ] 功能验证
  - [ ] nRF Connect能看到Manufacturer Data
  - [ ] Company ID正确 (0xFFF0)
  - [ ] MAC地址正确显示
  - [ ] BluFi配网流程正常工作

- [ ] iOS App集成
  - [ ] 实现Manufacturer Data解析代码
  - [ ] 测试MAC地址提取功能
  - [ ] 验证设备识别和匹配逻辑

- [ ] Android App集成
  - [ ] 实现统一的MAC获取方式
  - [ ] 保持向后兼容
  - [ ] 验证跨平台一致性

## 🎯 优势总结

1. **跨平台兼容**: iOS和Android统一使用Manufacturer Data
2. **无需连接**: 扫描阶段即可获取MAC地址
3. **符合标准**: 使用BLE标准的厂商数据字段
4. **向后兼容**: 不影响现有BluFi配网流程
5. **实现简洁**: 通过重写单个函数完成功能
6. **易于维护**: 代码集中，逻辑清晰

## 📚 相关文档

- [完整技术方案](./iOS-BLE-MAC-Address-Solution.md)
- [实施指南](./iOS-BLE-MAC-Implementation-Guide.md)
- [调试指南](./BluFi-Advertising-Debug-Guide.md)
- [BluFi配网说明](../docs/BluFi蓝牙配网说明文档.md)

## 🔧 故障排查

### 问题1: 编译错误
```
error: invalid conversion from 'const char*' to 'char*'
```
**解决**: 已修复，函数签名改为 `const char *name`

### 问题2: nRF Connect看不到Manufacturer Data
**检查**:
1. 查看日志确认配置成功
2. 确认设备在配网模式
3. 重新扫描刷新设备列表

### 问题3: MAC地址显示不正确
**检查**:
1. 确认字节序（Little Endian）
2. 确认读取的是BT MAC而不是WiFi MAC
3. 查看日志中的MAC地址输出

## 💡 后续优化建议

### 短期
1. 测试各种iOS/Android设备兼容性
2. 验证配网成功率无下降
3. 收集用户反馈

### 长期
1. 考虑申请正式的Company ID（当前使用测试ID 0xFFF0）
2. 支持通过Manufacturer Data传输更多设备信息
3. 实现广播数据的动态更新机制

---

**实施日期**: 2025-11-24
**实施者**: Development Team
**版本**: v1.0
**状态**: ✅ 代码实现完成，等待测试验证
