# iOS BLE MAC地址获取 - 实施指南

## 问题说明

- **问题**: iOS系统无法获取蓝牙设备的真实MAC地址(隐私保护机制)
- **现象**: BluFi配网时,设备名称显示为固定的 `BLUFI_DEVICE`,无法从设备名中解析MAC地址
- **需求**: iOS和Android需要统一的方式来识别设备

## 解决方案: Manufacturer Data

### 原理
在BLE广播包中添加 **Manufacturer Specific Data** 字段,携带设备MAC地址。

### 优势
✅ iOS和Android都能读取
✅ 无需建立BLE连接
✅ 符合BLE标准规范
✅ 不影响现有BluFi配网流程

## ESP32端实现 (已完成)

### 修改内容
文件: `main/boards/common/wifi_board.cc`

在 `ESP_BLUFI_EVENT_INIT_FINISH` 事件中添加了Manufacturer Data配置:

```cpp
// Manufacturer Data 格式:
// [0-1]: Company ID (0xFFF0)
// [2-7]: MAC Address (6 bytes)
uint8_t mfg_data[8];
mfg_data[0] = 0xF0;  // Company ID 低字节
mfg_data[1] = 0xFF;  // Company ID 高字节
memcpy(&mfg_data[2], mac_addr, 6);  // MAC地址

// 配置广播数据
esp_ble_adv_data_t adv_data = {};
adv_data.manufacturer_len = sizeof(mfg_data);
adv_data.p_manufacturer_data = mfg_data;
// ... 其他配置

esp_ble_gap_config_adv_data(&adv_data);
```

### 数据格式

```
Manufacturer Data (8字节):
┌────────┬────────┬──────────────────────────────┐
│ Byte 0 │ Byte 1 │ Bytes 2-7                    │
├────────┼────────┼──────────────────────────────┤
│  0xF0  │  0xFF  │ MAC Address (6 bytes)        │
│        │        │ 例: A1 B2 C3 D4 E5 F6        │
└────────┴────────┴──────────────────────────────┘

Company ID: 0xFFF0 (Little Endian)
用途: 自定义/测试用途
```

## iOS端实现

### Swift代码示例

```swift
import CoreBluetooth

class BLEManager: NSObject, CBCentralManagerDelegate {
    private var centralManager: CBCentralManager!

    override init() {
        super.init()
        centralManager = CBCentralManager(delegate: self, queue: nil)
    }

    func startScanning() {
        centralManager.scanForPeripherals(withServices: nil, options: nil)
    }

    // BLE扫描回调
    func centralManager(_ central: CBCentralManager,
                       didDiscover peripheral: CBPeripheral,
                       advertisementData: [String : Any],
                       rssi RSSI: NSNumber) {

        // 从 Manufacturer Data 读取MAC地址
        if let manufacturerData = advertisementData[CBAdvertisementDataManufacturerDataKey] as? Data {
            if let macAddress = extractMACAddress(from: manufacturerData) {
                print("✅ 设备MAC地址: \(macAddress)")
                print("   设备名称: \(peripheral.name ?? "Unknown")")
                print("   信号强度: \(RSSI) dBm")

                // 使用MAC地址进行设备识别和匹配
                handleDevice(mac: macAddress, peripheral: peripheral)
            }
        }
    }

    // 从Manufacturer Data提取MAC地址
    private func extractMACAddress(from data: Data) -> String? {
        // 检查数据长度
        guard data.count >= 8 else { return nil }

        // 检查Company ID (0xFFF0)
        let companyID = UInt16(data[0]) | (UInt16(data[1]) << 8)
        guard companyID == 0xFFF0 else { return nil }

        // 提取MAC地址
        let macBytes = data.subdata(in: 2..<8)
        let macAddress = macBytes.map { String(format: "%02X", $0) }
                                 .joined(separator: ":")

        return macAddress
    }

    // 处理发现的设备
    private func handleDevice(mac: String, peripheral: CBPeripheral) {
        // 根据业务需求处理:
        // 1. 与用户扫描的二维码中的MAC匹配
        // 2. 检查是否在设备白名单中
        // 3. 自动连接目标设备
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn {
            startScanning()
        }
    }
}
```

### Objective-C代码示例

```objective-c
#import <CoreBluetooth/CoreBluetooth.h>

@interface BLEManager : NSObject <CBCentralManagerDelegate>
@property (nonatomic, strong) CBCentralManager *centralManager;
@end

@implementation BLEManager

- (instancetype)init {
    if (self = [super init]) {
        _centralManager = [[CBCentralManager alloc] initWithDelegate:self queue:nil];
    }
    return self;
}

- (void)startScanning {
    [self.centralManager scanForPeripheralsWithServices:nil options:nil];
}

- (void)centralManager:(CBCentralManager *)central
 didDiscoverPeripheral:(CBPeripheral *)peripheral
     advertisementData:(NSDictionary<NSString *,id> *)advertisementData
                  RSSI:(NSNumber *)RSSI {

    // 读取 Manufacturer Data
    NSData *manufacturerData = advertisementData[CBAdvertisementDataManufacturerDataKey];
    if (manufacturerData && manufacturerData.length >= 8) {

        const uint8_t *bytes = (const uint8_t *)manufacturerData.bytes;

        // 检查 Company ID (0xFFF0)
        uint16_t companyID = bytes[0] | (bytes[1] << 8);
        if (companyID == 0xFFF0) {

            // 提取MAC地址
            NSString *macAddress = [NSString stringWithFormat:@"%02X:%02X:%02X:%02X:%02X:%02X",
                                   bytes[2], bytes[3], bytes[4],
                                   bytes[5], bytes[6], bytes[7]];

            NSLog(@"✅ 设备MAC地址: %@", macAddress);
            NSLog(@"   设备名称: %@", peripheral.name ?: @"Unknown");
            NSLog(@"   信号强度: %@ dBm", RSSI);

            // 使用MAC地址进行设备识别
            [self handleDeviceWithMAC:macAddress peripheral:peripheral];
        }
    }
}

- (void)handleDeviceWithMAC:(NSString *)mac peripheral:(CBPeripheral *)peripheral {
    // 设备识别和处理逻辑
}

- (void)centralManagerDidUpdateState:(CBCentralManager *)central {
    if (central.state == CBCentralManagerStatePoweredOn) {
        [self startScanning];
    }
}

@end
```

## Android端实现

### Kotlin代码示例

```kotlin
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult

class BLEManager {

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val scanRecord = result.scanRecord ?: return

            // 方式1: 从 Manufacturer Data 获取 (推荐,跨平台统一)
            val manufacturerData = scanRecord.getManufacturerSpecificData(0xFFF0)
            if (manufacturerData != null && manufacturerData.size == 6) {
                val macFromMfgData = manufacturerData.joinToString(":") {
                    String.format("%02X", it)
                }
                println("✅ MAC from Manufacturer Data: $macFromMfgData")
            }

            // 方式2: 直接获取 (Android特有,保持向后兼容)
            val macFromDevice = result.device.address
            println("   MAC from Device: $macFromDevice")

            // 使用MAC地址进行设备识别
            handleDevice(macFromMfgData ?: macFromDevice, result.device)
        }
    }

    private fun handleDevice(mac: String, device: BluetoothDevice) {
        // 设备识别和处理逻辑
    }
}
```

### Java代码示例

```java
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanRecord;

public class BLEManager {

    private final ScanCallback scanCallback = new ScanCallback() {
        @Override
        public void onScanResult(int callbackType, ScanResult result) {
            ScanRecord scanRecord = result.getScanRecord();
            if (scanRecord == null) return;

            // 从 Manufacturer Data 获取MAC
            byte[] manufacturerData = scanRecord.getManufacturerSpecificData(0xFFF0);
            if (manufacturerData != null && manufacturerData.length == 6) {
                String macAddress = String.format("%02X:%02X:%02X:%02X:%02X:%02X",
                    manufacturerData[0], manufacturerData[1], manufacturerData[2],
                    manufacturerData[3], manufacturerData[4], manufacturerData[5]);

                System.out.println("✅ MAC from Manufacturer Data: " + macAddress);

                // 使用MAC地址进行设备识别
                handleDevice(macAddress, result.getDevice());
            }
        }
    };

    private void handleDevice(String mac, BluetoothDevice device) {
        // 设备识别和处理逻辑
    }
}
```

## 测试验证

### 工具: nRF Connect

#### iOS/Android都可用
1. 下载安装 **nRF Connect** App
2. 打开App,开始扫描
3. 找到设备 `BLUFI_DEVICE`
4. 查看 **RAW** 或 **Manufacturer data** 字段
5. 验证格式: `F0 FF XX XX XX XX XX XX`

预期结果:
```
Device Name: BLUFI_DEVICE
Manufacturer Data: F0 FF A1 B2 C3 D4 E5 F6
                   ^^^^^ ^^^^^^^^^^^^^^^^^^^
                   公司ID  MAC地址(6字节)
```

### 真机测试清单

- [ ] ESP32编译烧录测试
- [ ] nRF Connect验证广播包
- [ ] iOS App测试MAC提取
- [ ] Android App测试MAC提取
- [ ] 验证配网流程完整性
- [ ] 跨平台MAC一致性验证

## 二维码配网集成

### ESP32端 (已有)
```cpp
// 二维码内容包含MAC地址
std::string qr_data = "deviceName=" + g_device_name + "&mac=" + mac_str;
display->ShowQrCode(qr_data.c_str());
```

### iOS端处理
```swift
func handleQRCode(content: String) {
    // 解析: deviceName=XIAOZHI_XXX&mac=A1:B2:C3:D4:E5:F6
    let params = parseQRParams(content)

    if let expectedMAC = params["mac"] {
        // 保存期望的MAC地址
        UserDefaults.standard.set(expectedMAC, forKey: "expectedDeviceMAC")

        // 开始BLE扫描,匹配此MAC
        startScanningForDevice(expectedMAC: expectedMAC)
    }
}

func startScanningForDevice(expectedMAC: String) {
    // 扫描时匹配Manufacturer Data中的MAC
    // 匹配成功后自动连接
}
```

## 注意事项

### 1. Company ID选择
- 当前使用: `0xFFF0` (测试/自定义用途)
- 生产环境建议: 申请正式的Company ID
- 申请地址: https://www.bluetooth.com/specifications/assigned-numbers/

### 2. 广播数据限制
- BLE广播包最大31字节
- Manufacturer Data占用8字节
- 剩余空间足够设备名称和其他必要信息

### 3. 隐私考虑
- MAC地址不是敏感信息
- Android本就公开MAC地址
- 仅用于设备识别,不涉及用户隐私

### 4. 兼容性
- iOS 5.0+ 支持 CoreBluetooth
- Android 4.3+ 支持 BLE
- 所有主流设备均支持Manufacturer Data

## 故障排查

### ESP32端

**问题1: 编译错误**
```
error: 'esp_ble_adv_data_t' has no member named 'manufacturer_len'
```
解决: 确保使用ESP-IDF 4.0+版本

**问题2: 广播失败**
```
E (xxx) WifiBoard: Config advertising data failed: ESP_ERR_INVALID_ARG
```
检查:
- Manufacturer Data长度是否正确
- 广播数据总大小是否超过31字节
- 是否在正确的时机调用(INIT_FINISH之后)

### iOS端

**问题1: 无法读取Manufacturer Data**
```swift
// 确认广播数据key正确
CBAdvertisementDataManufacturerDataKey // ✅ 正确
"manufacturerData" // ❌ 错误
```

**问题2: Company ID不匹配**
```swift
// 注意字节序 (Little Endian)
let companyID = UInt16(data[0]) | (UInt16(data[1]) << 8) // ✅ 正确
let companyID = UInt16(data[1]) | (UInt16(data[0]) << 8) // ❌ 错误
```

### Android端

**问题1: 获取不到Manufacturer Data**
```kotlin
// 使用正确的Company ID
scanRecord.getManufacturerSpecificData(0xFFF0) // ✅ 正确
scanRecord.getManufacturerSpecificData(0xFFFF) // ❌ 错误
```

## 参考文档

- [完整技术方案](./iOS-BLE-MAC-Address-Solution.md)
- [ESP-IDF BLE GAP API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/bluetooth/esp_gap_ble.html)
- [Apple CoreBluetooth](https://developer.apple.com/documentation/corebluetooth)
- [Android BLE Guide](https://developer.android.com/guide/topics/connectivity/bluetooth/ble-overview)

---

**版本**: v1.0
**更新日期**: 2025-11-24
**实施状态**: ESP32端已完成,等待iOS/Android端集成
