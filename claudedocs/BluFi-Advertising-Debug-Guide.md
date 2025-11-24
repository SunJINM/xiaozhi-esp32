# BluFi Manufacturer Data 调试指南

## 问题描述

在 `ESP_BLUFI_EVENT_INIT_FINISH` 事件中配置 Manufacturer Data 可能不生效，因为：
1. BluFi可能已经使用默认参数配置了广播
2. 广播数据配置是异步的，需要等待完成事件
3. BluFi内部可能覆盖了自定义的广播配置

## 解决方案

### 方案1: 当前实现（已采用）

在 `ESP_BLUFI_EVENT_INIT_FINISH` 中配置，并添加延迟：

```cpp
// 配置广播数据
static uint8_t mfg_data[8];  // 使用static确保生命周期
// ... 填充数据 ...

esp_ble_gap_config_adv_data(&adv_data);

// 等待配置完成
vTaskDelay(pdMS_TO_TICKS(100));

esp_blufi_adv_start();
```

**验证方法**: 查看日志中是否有成功配置的提示

### 方案2: 监听GAP事件（推荐）

如果方案1不生效，需要监听 `ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT` 事件：

#### 步骤1: 添加全局标志
```cpp
// wifi_board.cc 文件顶部
static bool adv_data_configured = false;
static uint8_t g_manufacturer_data[8];  // 全局存储
```

#### 步骤2: 实现GAP事件处理器

```cpp
// 在 wifi_board.cc 中添加
static void ble_gap_event_handler(esp_gap_ble_cb_event_t event,
                                   esp_ble_gap_cb_param_t *param) {
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "Advertising data set complete, status: %d",
                 param->adv_data_cmpl.status);

        if (param->adv_data_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            adv_data_configured = true;
            ESP_LOGI(TAG, "✅ Manufacturer Data configured successfully");

            // 现在可以安全地启动广播
            if (!ble_is_connected) {
                esp_blufi_adv_start();
            }
        } else {
            ESP_LOGE(TAG, "❌ Advertising data configuration failed: %d",
                     param->adv_data_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        ESP_LOGI(TAG, "Advertising started, status: %d",
                 param->adv_start_cmpl.status);
        break;

    default:
        break;
    }
}
```

#### 步骤3: 注册GAP回调

```cpp
// 在 EnterWifiConfigMode() 中，BluFi初始化后注册
ret = esp_ble_gap_register_callback(ble_gap_event_handler);
if (ret) {
    ESP_LOGE(TAG, "GAP callback register failed: %s", esp_err_to_name(ret));
}
```

#### 步骤4: 修改INIT_FINISH事件处理

```cpp
case ESP_BLUFI_EVENT_INIT_FINISH:
    ESP_LOGI(TAG, "BLUFI init finish");

    // 设置设备名称
    if (!g_device_name.empty()) {
        esp_ble_gap_set_device_name(g_device_name.c_str());
    }

    // 准备Manufacturer Data
    uint8_t mac_addr[6];
    if (esp_read_mac(mac_addr, ESP_MAC_BT) == ESP_OK) {
        g_manufacturer_data[0] = 0xF0;
        g_manufacturer_data[1] = 0xFF;
        memcpy(&g_manufacturer_data[2], mac_addr, 6);

        // 配置广播数据
        esp_ble_adv_data_t adv_data = {};
        adv_data.set_scan_rsp = false;
        adv_data.include_name = true;
        adv_data.include_txpower = false;
        adv_data.manufacturer_len = sizeof(g_manufacturer_data);
        adv_data.p_manufacturer_data = g_manufacturer_data;
        adv_data.flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);

        esp_ble_gap_config_adv_data(&adv_data);

        ESP_LOGI(TAG, "Manufacturer Data configuration initiated...");
        // 不要在这里调用 esp_blufi_adv_start()
        // 等待 ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT 事件
    }
    break;
```

### 方案3: 延迟启动广播（简单但不优雅）

如果GAP事件处理太复杂，可以简单地延迟启动：

```cpp
case ESP_BLUFI_EVENT_INIT_FINISH:
    ESP_LOGI(TAG, "BLUFI init finish");

    // ... 配置代码 ...

    esp_ble_gap_config_adv_data(&adv_data);

    // 延迟更长时间确保配置完成
    vTaskDelay(pdMS_TO_TICKS(500));  // 500ms

    esp_blufi_adv_start();
    break;
```

### 方案4: 修改BluFi源码（最彻底）

如果以上方案都不行，可能需要修改ESP-IDF中的BluFi实现：

1. 找到BluFi的广播启动代码（在ESP-IDF组件中）
2. 在启动前插入Manufacturer Data配置
3. 重新编译ESP-IDF

位置参考:
```
components/bt/common/btc/profile/esp/blufi/blufi_prf.c
或
components/bt/host/nimble/nimble/apps/blufi/src/blufi_priv.h
```

## 调试步骤

### 1. 查看编译日志
```bash
idf.py build | grep -i "manufacturer\|advertising"
```

### 2. 查看运行日志

期望看到的日志序列：
```
I (xxx) WifiBoard: BLUFI init finish
I (xxx) WifiBoard: Device name set to XIAOZHI_XXXXXXXXXXXX
I (xxx) WifiBoard: Manufacturer Data configured with MAC: AA:BB:CC:DD:EE:FF
I (xxx) WifiBoard: iOS devices can now read MAC address from Manufacturer Data
I (yyy) WifiBoard: Advertising data set complete, status: 0  # 方案2才有
```

如果看到错误：
```
E (xxx) WifiBoard: Config advertising data failed: ESP_ERR_INVALID_STATE
```
说明时机不对，需要采用方案2。

### 3. 使用nRF Connect验证

**iOS/Android步骤**:
1. 安装 **nRF Connect** App
2. 开始扫描BLE设备
3. 找到 `BLUFI_DEVICE` 设备
4. 点击查看详情
5. 切换到 **RAW** 或 **Advertising Data** 标签页
6. 查找 **Manufacturer data** 字段

**预期结果**:
```
Manufacturer data (8 bytes):
F0 FF AA BB CC DD EE FF

解析:
- Company ID: 0xFFF0 (F0 FF in Little Endian)
- MAC Address: AA:BB:CC:DD:EE:FF
```

**如果看不到 Manufacturer data 字段**:
- 说明配置未生效
- 需要尝试方案2或方案3

### 4. 使用ESP-IDF Monitor抓取日志

```bash
idf.py monitor | tee blufi_debug.log
```

重点关注：
- GAP事件日志
- BluFi事件序列
- 广播配置返回值

### 5. 使用蓝牙抓包工具（高级）

**Android (需要root)**:
```bash
adb shell
btsnoop_net on
```

**iOS**:
- 安装 Xcode
- 使用 Hardware → Bluetooth → Packet Logger

**Wireshark分析**:
1. 打开抓包文件
2. 过滤: `bthci_evt.code == 0x3e`
3. 查找 Advertising Report
4. 检查 Manufacturer Specific Data (Type 0xFF)

## 常见问题

### Q1: 编译错误 - 找不到 esp_ble_gap_cb_param_t

**原因**: ESP-IDF版本不匹配

**解决**:
```cpp
// 检查ESP-IDF版本
#include "esp_idf_version.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 0, 0)
// 使用新API
#else
// 使用旧API
#endif
```

### Q2: 广播数据总是显示为空

**可能原因**:
1. `mfg_data` 变量作用域问题 → 使用 `static`
2. 配置时机太早 → 添加延迟或监听事件
3. BluFi覆盖了配置 → 使用方案2监听GAP事件

### Q3: iOS仍然读取不到MAC地址

**检查清单**:
- [ ] nRF Connect能看到Manufacturer Data吗？
- [ ] Company ID是否正确 (0xFFF0)?
- [ ] iOS代码是否正确解析Little Endian?
- [ ] iOS是否有蓝牙权限？

### Q4: 配置成功但配网失败

**原因**: 广播数据配置可能影响了BluFi的其他字段

**解决**: 确保配置中包含BluFi需要的所有字段
```cpp
adv_data.set_scan_rsp = false;
adv_data.include_name = true;      // BluFi需要
adv_data.include_txpower = false;
// 添加Service UUID（如果BluFi需要）
```

## 推荐实施顺序

1. **先尝试当前方案** (方案1 + 延迟)
   - 简单直接
   - 大多数情况下有效

2. **使用nRF Connect验证**
   - 确认问题是配置失败还是App读取问题

3. **如果配置失败，实施方案2**
   - 监听GAP事件
   - 更可靠但代码稍复杂

4. **最后考虑方案3或方案4**
   - 延迟启动（不优雅）
   - 修改BluFi源码（维护成本高）

## 参考资料

- [ESP-IDF BLE GAP API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/bluetooth/esp_gap_ble.html)
- [ESP-IDF BluFi Example](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/blufi)
- [BLE Advertising Data Format](https://www.bluetooth.com/specifications/assigned-numbers/generic-access-profile/)

---

**更新日期**: 2025-11-24
**适用版本**: ESP-IDF v4.0+
