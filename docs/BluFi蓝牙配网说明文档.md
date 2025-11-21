# BluFi 蓝牙配网技术说明文档

## 目录
1. [概述](#概述)
2. [配网流程](#配网流程)
3. [安全机制](#安全机制)
4. [协议详解](#协议详解)
5. [数据交互](#数据交互)
6. [代码实现](#代码实现)
7. [使用指南](#使用指南)
8. [故障排查](#故障排查)

---

## 概述

### 什么是 BluFi

BluFi 是 Espressif 开发的一种基于蓝牙低功耗(BLE)的 Wi-Fi 配网协议。它允许用户通过手机 App 通过蓝牙连接将 Wi-Fi 凭证(SSID 和密码)安全地传输到 ESP32 设备,从而完成设备的网络配置。

### 技术优势

- **无需预配置网络**: 设备首次启动无需连接到任何 Wi-Fi 网络
- **安全传输**: 采用 DH 密钥交换和 AES 加密保护 Wi-Fi 凭证
- **低功耗**: 基于 BLE 技术,功耗更低
- **跨平台支持**: 支持 Android 和 iOS 平台
- **用户友好**: 通过手机 App 即可完成配置,无需复杂操作

### 应用场景

本项目的 BluFi 配网实现主要应用于:
- 设备首次启动时自动配网
- 设备无法连接已保存的 Wi-Fi 时的故障恢复
- 用户主动重置网络配置

---

## 配网流程

### 整体流程图

```
┌─────────────┐                    ┌─────────────┐
│             │                    │             │
│  ESP32设备  │                    │  手机 App   │
│             │                    │             │
└──────┬──────┘                    └──────┬──────┘
       │                                  │
       │ 1. 初始化蓝牙控制器                  │
       │────────────────────────>         │
       │                                  │
       │ 2. 启动 BluFi 服务                 │
       │────────────────────────>         │
       │                                  │
       │ 3. 开始 BLE 广播                   │
       │    (设备名: XIAOZHI_XXXXXX)       │
       │────────────────────────>         │
       │                                  │
       │                    4. 扫描并连接设备│
       │                    <────────────────│
       │                                  │
       │ 5. BLE 连接建立                    │
       │<────────────────────────────────>│
       │                                  │
       │ 6. 停止广播,初始化安全层              │
       │────────────────────────>         │
       │                                  │
       │ 7. DH 密钥协商                     │
       │<────────────────────────────────>│
       │   - 交换 DH 参数                  │
       │   - 计算共享密钥                  │
       │   - 生成 AES 加密密钥              │
       │                                  │
       │ 8. 接收加密的 SSID                 │
       │<─────────────────────────────────│
       │                                  │
       │ 9. 接收加密的密码                  │
       │<─────────────────────────────────│
       │                                  │
       │ 10. 连接请求                      │
       │<─────────────────────────────────│
       │                                  │
       │ 11. 尝试连接 Wi-Fi                │
       │────────────────────────>         │
       │                                  │
       │ 12. 连接成功/失败通知              │
       │─────────────────────────────────>│
       │                                  │
       │ 13. 保存凭证到 NVS                │
       │────────────────────────>         │
       │                                  │
       │ 14. 清理蓝牙资源                  │
       │────────────────────────>         │
       │                                  │
       │ 15. 进入正常工作模式               │
       │────────────────────────>         │
       │                                  │
```

### 详细步骤说明

#### 第一阶段: 初始化 (代码位置: wifi_board.cc:164-221)

1. **内存检查**
   - 检查可用堆内存是否充足 (至少 100KB)
   - 防止内存不足导致初始化失败

2. **NVS 初始化**
   ```cpp
   esp_err_t ret = nvs_flash_init();
   if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
       ESP_ERROR_CHECK(nvs_flash_erase());
       ret = nvs_flash_init();
   }
   ```
   - 初始化非易失性存储,用于保存 Wi-Fi 凭证

3. **网络接口初始化**
   ```cpp
   ESP_ERROR_CHECK(esp_netif_init());
   wifi_event_group = xEventGroupCreate();
   esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
   ```
   - 创建 Wi-Fi STA 模式的网络接口
   - 创建事件组用于同步 Wi-Fi 连接状态

4. **Wi-Fi 事件处理器注册**
   ```cpp
   ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
   ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));
   ```
   - 注册 Wi-Fi 连接、断开等事件的处理函数
   - 注册获取 IP 地址的事件处理函数

5. **蓝牙控制器初始化**
   ```cpp
   esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
   esp_bt_controller_init(&bt_cfg);
   esp_bt_controller_enable(ESP_BT_MODE_BLE);
   ```
   - 初始化蓝牙控制器为 BLE 模式
   - 启用蓝牙功能

6. **BluFi 主机和回调初始化**
   ```cpp
   ret = esp_blufi_host_and_cb_init(&blufi_callbacks);
   ```
   - 初始化 Bluedroid/NimBLE 蓝牙主机栈
   - 注册 BluFi 事件回调函数
   - 初始化 GATT 服务

#### 第二阶段: 设备名称和二维码生成 (代码位置: wifi_board.cc:223-255)

1. **读取蓝牙 MAC 地址**
   ```cpp
   uint8_t mac_addr[6];
   esp_err_t mac_ret = esp_read_mac(mac_addr, ESP_MAC_BT);
   ```
   - 获取设备的蓝牙 MAC 地址用于生成唯一设备名

2. **生成设备名称**
   ```cpp
   g_device_name = "XIAOZHI_" + std::string(mac_str_for_name);
   // 例如: XIAOZHI_A1B2C3D4E5F6
   ```
   - 设备名称格式: `XIAOZHI_` + 12位MAC地址(大写十六进制)
   - 确保每个设备都有唯一的识别名称

3. **生成配网二维码**
   ```cpp
   std::string qr_data = "deviceName=" + g_device_name + "&mac=" + mac_str_for_qr;
   display->ShowQrCode(qr_data.c_str());
   ```
   - 二维码内容包含设备名称和 MAC 地址
   - 用户可扫描二维码快速识别设备

4. **设置 BLE 设备名称**
   - 在 `ESP_BLUFI_EVENT_INIT_FINISH` 事件中设置
   - 使用 `esp_ble_gap_set_device_name()` 设置蓝牙广播名称

#### 第三阶段: 蓝牙连接和安全协商 (代码位置: wifi_board.cc:92-162)

1. **BluFi 初始化完成事件** (`ESP_BLUFI_EVENT_INIT_FINISH`)
   ```cpp
   esp_ble_gap_set_device_name(g_device_name.c_str());
   esp_blufi_adv_start();
   ```
   - 设置设备名称
   - 开始 BLE 广播,使设备可被发现

2. **BLE 连接建立事件** (`ESP_BLUFI_EVENT_BLE_CONNECT`)
   ```cpp
   ble_is_connected = true;
   esp_blufi_adv_stop();      // 停止广播
   blufi_security_init();      // 初始化安全层
   ```
   - 停止广播以节省资源
   - 初始化安全加密环境

3. **安全协商过程** (代码位置: blufi_security.cc:66-172)

   **3.1 接收 DH 参数长度** (`SEC_TYPE_DH_PARAM_LEN`)
   ```cpp
   blufi_sec->dh_param_len = ((data[1]<<8)|data[2]);
   blufi_sec->dh_param = (uint8_t *)malloc(blufi_sec->dh_param_len);
   ```
   - 手机端发送 DH 参数的长度信息
   - 设备分配内存准备接收参数

   **3.2 接收并处理 DH 参数** (`SEC_TYPE_DH_PARAM_DATA`)
   ```cpp
   // 读取 DH 参数
   mbedtls_dhm_read_params(&blufi_sec->dhm, &param, &param[blufi_sec->dh_param_len]);

   // 生成设备端公钥
   mbedtls_dhm_make_public(&blufi_sec->dhm, dhm_len,
                           blufi_sec->self_public_key,
                           DH_SELF_PUB_KEY_LEN, myrand, NULL);

   // 计算共享密钥
   mbedtls_dhm_calc_secret(&blufi_sec->dhm,
                          blufi_sec->share_key,
                          SHARE_KEY_LEN,
                          &blufi_sec->share_len,
                          myrand, NULL);

   // 生成 AES 密钥 (PSK)
   mbedtls_md5(blufi_sec->share_key, blufi_sec->share_len, blufi_sec->psk);

   // 设置 AES 加密密钥
   mbedtls_aes_setkey_enc(&blufi_sec->aes, blufi_sec->psk, PSK_LEN * 8);
   ```
   - 完整的 Diffie-Hellman 密钥交换流程
   - 使用 MD5 将共享密钥转换为 128 位 AES 密钥
   - 将设备公钥返回给手机端

#### 第四阶段: Wi-Fi 凭证传输 (代码位置: wifi_board.cc:148-157)

1. **接收 SSID** (`ESP_BLUFI_EVENT_RECV_STA_SSID`)
   ```cpp
   strncpy((char *)sta_config.sta.ssid,
           (char *)param->sta_ssid.ssid,
           param->sta_ssid.ssid_len);
   sta_config.sta.ssid[param->sta_ssid.ssid_len] = '\0';
   ```
   - 接收经 AES 加密的 SSID
   - BluFi 框架自动解密后传递给回调函数

2. **接收密码** (`ESP_BLUFI_EVENT_RECV_STA_PASSWD`)
   ```cpp
   strncpy((char *)sta_config.sta.password,
           (char *)param->sta_passwd.passwd,
           param->sta_passwd.passwd_len);
   sta_config.sta.password[param->sta_passwd.passwd_len] = '\0';
   ```
   - 接收经 AES 加密的密码
   - 出于安全考虑,不记录密码到日志

#### 第五阶段: Wi-Fi 连接和结果反馈 (代码位置: wifi_board.cc:127-132, 267-308)

1. **连接请求** (`ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP`)
   ```cpp
   esp_wifi_disconnect();
   esp_wifi_set_config(WIFI_IF_STA, &sta_config);
   esp_wifi_connect();
   ```
   - 断开当前连接
   - 设置新的 Wi-Fi 配置
   - 尝试连接到目标网络

2. **等待连接结果**
   ```cpp
   EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                          CONNECTED_BIT | FAILED_BIT,
                                          pdTRUE, pdFALSE, portMAX_DELAY);
   ```
   - 通过事件组等待连接成功或失败

3. **连接成功处理** (`CONNECTED_BIT`)
   ```cpp
   // 保存凭证
   ssid_manager.AddSsid(reinterpret_cast<const char*>(sta_config.sta.ssid),
                        reinterpret_cast<const char*>(sta_config.sta.password));

   // 发送成功消息 (JSON 格式)
   {
     "type": 4,
     "result": true,
     "data": {
       "progress": 100,
       "ssid": "实际SSID"
     }
   }

   // 清理蓝牙资源
   esp_blufi_host_deinit();
   esp_bt_controller_disable();
   esp_bt_controller_deinit();
   ```
   - 将 SSID 和密码保存到 NVS
   - 向手机发送成功通知
   - 释放蓝牙资源以节省内存

4. **连接失败处理** (`FAILED_BIT`)
   ```cpp
   const char *json_str_fail = "wifi connect fail";
   esp_blufi_send_custom_data((uint8_t*)json_str_fail, strlen(json_str_fail));
   ```
   - 向手机发送失败通知
   - 继续等待用户重试

---

## 安全机制

### 密钥交换: Diffie-Hellman (DH) 算法

#### DH 算法原理

Diffie-Hellman 密钥交换是一种安全的密钥协商算法,允许双方在不安全的通道上建立共享密钥。

**数学基础:**
```
给定质数 p 和生成元 g (由手机端生成并发送)

手机端:
1. 生成随机私钥 a
2. 计算公钥 A = g^a mod p
3. 发送 A 给设备

设备端:
1. 生成随机私钥 b
2. 计算公钥 B = g^b mod p
3. 发送 B 给手机
4. 计算共享密钥 K = A^b mod p

手机端:
1. 计算共享密钥 K = B^a mod p

结果: 双方得到相同的共享密钥 K = g^(ab) mod p
中间人无法从 A 和 B 计算出 K (离散对数难题)
```

#### 实现细节

**密钥长度配置:**
```cpp
#define DH_SELF_PUB_KEY_LEN     128  // 设备公钥长度: 128 字节 (1024位)
#define SHARE_KEY_LEN           128  // 共享密钥长度: 128 字节
#define PSK_LEN                 16   // AES 密钥长度: 16 字节 (128位)
```

**DH 上下文结构:**
```cpp
struct blufi_security {
    uint8_t  self_public_key[DH_SELF_PUB_KEY_LEN];  // 设备公钥
    uint8_t  share_key[SHARE_KEY_LEN];              // 共享密钥
    size_t   share_len;                             // 共享密钥实际长度
    uint8_t  psk[PSK_LEN];                          // AES 预共享密钥
    uint8_t  *dh_param;                             // DH 参数缓冲区
    int      dh_param_len;                          // DH 参数长度
    uint8_t  iv[16];                                // AES 初始化向量
    mbedtls_dhm_context dhm;                        // mbedTLS DH 上下文
    mbedtls_aes_context aes;                        // mbedTLS AES 上下文
};
```

**协商流程代码:**
```cpp
// 1. 接收 DH 参数 (p, g, 手机公钥)
mbedtls_dhm_read_params(&blufi_sec->dhm, &param, &param[blufi_sec->dh_param_len]);

// 2. 生成设备私钥和公钥 (使用硬件随机数生成器)
mbedtls_dhm_make_public(&blufi_sec->dhm, dhm_len,
                        blufi_sec->self_public_key,
                        DH_SELF_PUB_KEY_LEN,
                        myrand,    // 随机数生成函数
                        NULL);

// 3. 计算共享密钥
mbedtls_dhm_calc_secret(&blufi_sec->dhm,
                       blufi_sec->share_key,
                       SHARE_KEY_LEN,
                       &blufi_sec->share_len,
                       myrand, NULL);

// 4. 派生 AES 密钥 (使用 MD5 哈希)
mbedtls_md5(blufi_sec->share_key, blufi_sec->share_len, blufi_sec->psk);

// 5. 设置 AES 加密上下文
mbedtls_aes_setkey_enc(&blufi_sec->aes, blufi_sec->psk, PSK_LEN * 8);
```

**随机数生成:**
```cpp
static int myrand(void *rng_state, unsigned char *output, size_t len) {
    esp_fill_random(output, len);  // 使用 ESP32 硬件随机数生成器
    return 0;
}
```

### 加密传输: AES-128 CFB 模式

#### AES 加密模式选择

**CFB (Cipher Feedback) 模式特点:**
- 流式加密,不需要填充
- 适合加密任意长度的数据
- IV (初始化向量) 确保相同明文产生不同密文
- 加密和解密使用相同的 AES 密钥调度

#### 加密实现

```cpp
int blufi_aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len) {
    int ret;
    size_t iv_offset = 0;
    uint8_t iv0[16];

    if (!blufi_sec) {
        return -1;
    }

    // 复制基础 IV 并设置序列号
    memcpy(iv0, blufi_sec->iv, sizeof(blufi_sec->iv));
    iv0[0] = iv8;   // 每个数据包使用不同的 IV

    // AES-128 CFB128 加密 (原地加密)
    ret = mbedtls_aes_crypt_cfb128(&blufi_sec->aes,
                                   MBEDTLS_AES_ENCRYPT,  // 加密模式
                                   crypt_len,            // 数据长度
                                   &iv_offset,           // IV 偏移量
                                   iv0,                  // 初始化向量
                                   crypt_data,           // 输入数据
                                   crypt_data);          // 输出数据(原地)
    if (ret) {
        return -1;
    }

    return crypt_len;
}
```

#### 解密实现

```cpp
int blufi_aes_decrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len) {
    int ret;
    size_t iv_offset = 0;
    uint8_t iv0[16];

    if (!blufi_sec) {
        return -1;
    }

    // 复制基础 IV 并设置序列号
    memcpy(iv0, blufi_sec->iv, sizeof(blufi_sec->iv));
    iv0[0] = iv8;   // 必须与加密时使用相同的 IV

    // AES-128 CFB128 解密 (原地解密)
    ret = mbedtls_aes_crypt_cfb128(&blufi_sec->aes,
                                   MBEDTLS_AES_DECRYPT,  // 解密模式
                                   crypt_len,            // 数据长度
                                   &iv_offset,           // IV 偏移量
                                   iv0,                  // 初始化向量
                                   crypt_data,           // 输入数据
                                   crypt_data);          // 输出数据(原地)
    if (ret) {
        return -1;
    }

    return crypt_len;
}
```

**IV 管理策略:**
- 基础 IV 初始化为全 0: `memset(blufi_sec->iv, 0x0, sizeof(blufi_sec->iv))`
- 每个数据包使用 `iv8` 参数区分,防止 IV 重用
- `iv8` 由 BluFi 协议层管理,通常是数据包序列号

### 数据完整性: CRC-16

#### CRC 校验实现

```cpp
uint16_t blufi_crc_checksum(uint8_t iv8, uint8_t *data, int len) {
    // iv8 参数被忽略,未使用
    return esp_crc16_be(0, data, len);  // 使用 ESP-IDF 的 CRC-16 BE 实现
}
```

**CRC-16 特性:**
- 算法: CRC-16-CCITT (Big Endian)
- 多项式: 0x1021
- 初始值: 0x0000
- 用途: 检测数据传输错误,但不提供加密保护

### 安全性分析

#### 优势

1. **密钥安全性**
   - DH 算法确保共享密钥仅由双方知道
   - 使用 1024 位 DH 参数,提供足够的安全强度
   - 硬件随机数生成器增强随机性

2. **传输安全性**
   - AES-128 提供强加密保护
   - CFB 模式适合流式数据
   - IV 机制防止密文重放攻击

3. **完整性保护**
   - CRC-16 校验检测传输错误
   - BluFi 协议层提供额外的序列号验证

#### 潜在风险和缓解措施

1. **中间人攻击 (MITM)**
   - **风险**: DH 算法本身不提供身份认证
   - **缓解**: 通过二维码传递设备名称和 MAC 地址,用户可验证设备身份
   - **建议**: 生产环境可考虑增加证书或预共享密钥验证

2. **密钥重用**
   - **风险**: 同一密钥长时间使用可能增加破解风险
   - **缓解**: 每次配网重新协商密钥,配网完成后销毁密钥
   - **实现**: `blufi_security_deinit()` 彻底清除密钥材料

3. **MD5 哈希**
   - **风险**: MD5 存在碰撞漏洞,不推荐用于安全场景
   - **影响**: 此处用于密钥派生,实际风险较低 (攻击者无法控制输入)
   - **建议**: 考虑升级到 SHA-256 或其他现代哈希算法

4. **内存安全**
   - **保护**: 所有密钥在使用后清零并释放
   ```cpp
   memset(blufi_sec, 0x0, sizeof(struct blufi_security));
   free(blufi_sec);
   blufi_sec = NULL;
   ```

---

## 协议详解

### BluFi 协议层次

```
┌─────────────────────────────────────┐
│      应用层 (Application)            │  WiFi 凭证、状态报告
├─────────────────────────────────────┤
│      BluFi 协议层 (Protocol)         │  数据封装、序列号、类型
├─────────────────────────────────────┤
│      安全层 (Security)               │  DH、AES、CRC
├─────────────────────────────────────┤
│      GATT 服务层 (GATT Service)      │  UUID、特征值
├─────────────────────────────────────┤
│      蓝牙层 (BLE)                    │  广播、连接、通知
└─────────────────────────────────────┘
```

### BluFi 回调事件

系统通过注册回调函数处理各种 BluFi 事件:

```cpp
static esp_blufi_callbacks_t blufi_callbacks = {
    .event_cb = blufi_event_callback,                    // 主事件回调
    .negotiate_data_handler = blufi_dh_negotiate_data_handler,  // DH 协商
    .encrypt_func = blufi_aes_encrypt,                   // 加密函数
    .decrypt_func = blufi_aes_decrypt,                   // 解密函数
    .checksum_func = blufi_crc_checksum,                 // 校验和函数
};
```

#### 主要事件类型

| 事件常量 | 触发时机 | 处理动作 |
|---------|---------|---------|
| `ESP_BLUFI_EVENT_INIT_FINISH` | BluFi 初始化完成 | 设置设备名称,启动广播 |
| `ESP_BLUFI_EVENT_DEINIT_FINISH` | BluFi 反初始化完成 | 清理资源 |
| `ESP_BLUFI_EVENT_BLE_CONNECT` | BLE 连接建立 | 停止广播,初始化安全层 |
| `ESP_BLUFI_EVENT_BLE_DISCONNECT` | BLE 连接断开 | 清理安全层,重新开始广播 |
| `ESP_BLUFI_EVENT_SET_WIFI_OPMODE` | 设置 Wi-Fi 模式 | 配置为 STA/AP/STA+AP 模式 |
| `ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP` | 请求连接 AP | 使用接收的凭证连接 Wi-Fi |
| `ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP` | 请求断开 AP | 断开当前 Wi-Fi 连接 |
| `ESP_BLUFI_EVENT_REPORT_ERROR` | 报告错误 | 发送错误信息给手机 |
| `ESP_BLUFI_EVENT_GET_WIFI_STATUS` | 获取 Wi-Fi 状态 | 返回当前连接状态 |
| `ESP_BLUFI_EVENT_RECV_STA_SSID` | 接收 SSID | 保存到配置结构体 |
| `ESP_BLUFI_EVENT_RECV_STA_PASSWD` | 接收密码 | 保存到配置结构体 |

### GATT 服务结构

BluFi 使用自定义的 GATT 服务和特征值:

**服务 UUID**: `0xFFFF` (自定义服务)

**特征值**:
- **Write**: 用于接收来自手机的数据 (SSID、密码、控制命令)
- **Notify**: 用于向手机发送状态通知和响应

### 数据包格式

BluFi 协议数据包的通用格式:

```
┌──────┬──────┬──────────┬──────────┬──────────────┬──────────┐
│ Type │ FC   │ Sequence │  Length  │   Data       │ Checksum │
├──────┼──────┼──────────┼──────────┼──────────────┼──────────┤
│ 1B   │ 1B   │   1B     │   1B     │   Variable   │   2B     │
└──────┴──────┴──────────┴──────────┴──────────────┴──────────┘

Type (类型字段):
  - Bit 7-6: 数据方向 (00=请求, 01=响应)
  - Bit 5-0: 数据类型 (SSID, 密码, 状态等)

FC (Frame Control - 帧控制):
  - Bit 0: 是否加密
  - Bit 1: 是否有校验和
  - Bit 2: 数据方向
  - Bit 3-7: 保留

Sequence (序列号): 用于排序和防止重放

Length (长度): 数据段的字节数

Data (数据): 实际传输的内容 (加密/明文)

Checksum (校验和): CRC-16 校验值
```

### 安全协商数据类型

密钥交换过程中使用的自定义数据类型:

```cpp
#define SEC_TYPE_DH_PARAM_LEN   0x00  // DH 参数长度
#define SEC_TYPE_DH_PARAM_DATA  0x01  // DH 参数数据 (p, g, 公钥)
#define SEC_TYPE_DH_P           0x02  // DH 质数 p (单独发送)
#define SEC_TYPE_DH_G           0x03  // DH 生成元 g (单独发送)
#define SEC_TYPE_DH_PUBLIC      0x04  // DH 公钥 (单独发送)
```

---

## 数据交互

### 配网数据流向图

```
手机 App                                    ESP32 设备
   │                                           │
   │──────── 连接 BLE ─────────────────────────>│
   │                                           │
   │<──────── 连接成功 ──────────────────────────│
   │                                           │
   │──── SEC_TYPE_DH_PARAM_LEN ───────────────>│
   │     (DH 参数长度: 0x0100)                  │
   │                                           │
   │──── SEC_TYPE_DH_PARAM_DATA ──────────────>│
   │     (p, g, 手机公钥)                       │
   │                                           │
   │<────── 设备公钥 ────────────────────────────│
   │     (DH 公钥交换完成)                      │
   │                                           │
   │──── ESP_BLUFI_EVENT_RECV_STA_SSID ───────>│
   │     (加密的 SSID)                          │
   │                                           │
   │──── ESP_BLUFI_EVENT_RECV_STA_PASSWD ─────>│
   │     (加密的密码)                           │
   │                                           │
   │──── ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP ───>│
   │     (连接请求)                             │
   │                                           │
   │                                     [连接 Wi-Fi...]
   │                                           │
   │<──── 自定义数据 (JSON) ─────────────────────│
   │     {"type":4,"result":true,...}           │
   │                                           │
   │──────── 断开 BLE ─────────────────────────>│
   │                                           │
```

### 状态报告 JSON 格式

**连接成功通知:**
```json
{
  "type": 4,
  "result": true,
  "data": {
    "progress": 100,
    "ssid": "MyWiFiNetwork"
  }
}
```

**连接失败通知:**
```
"wifi connect fail"
```
(简单字符串格式)

### Wi-Fi 事件处理

```cpp
// IP 地址获取事件
static void ip_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data) {
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Got IP address, setting CONNECTED_BIT");
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);  // 通知主线程
    }
}

// Wi-Fi 断开事件
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "WIFI_EVENT_STA_DISCONNECTED");
        xEventGroupSetBits(wifi_event_group, FAILED_BIT);  // 通知主线程失败
    }
}
```

### 二维码数据格式

**标准格式:**
```
deviceName=XIAOZHI_A1B2C3D4E5F6&mac=A1:B2:C3:D4:E5:F6
```

**字段说明:**
- `deviceName`: BLE 广播名称,用于连接识别
- `mac`: 蓝牙 MAC 地址,用于设备唯一标识

**生成代码:**
```cpp
char mac_str_for_name[13];
sprintf(mac_str_for_name, "%02X%02X%02X%02X%02X%02X",
        mac_addr[0], mac_addr[1], mac_addr[2],
        mac_addr[3], mac_addr[4], mac_addr[5]);

g_device_name = "XIAOZHI_" + std::string(mac_str_for_name);

char mac_str_for_qr[18];
sprintf(mac_str_for_qr, "%02X:%02X:%02X:%02X:%02X:%02X",
        mac_addr[0], mac_addr[1], mac_addr[2],
        mac_addr[3], mac_addr[4], mac_addr[5]);

std::string qr_data = "deviceName=" + g_device_name + "&mac=" + mac_str_for_qr;
```

---

## 代码实现

### 核心文件结构

```
main/boards/common/
├── wifi_board.h              # WifiBoard 类声明
├── wifi_board.cc             # Wi-Fi 和 BluFi 主逻辑
├── blufi.h                   # BluFi 函数声明
├── blufi_security.cc         # 安全层实现 (DH, AES, CRC)
└── blufi_init.cc             # 蓝牙主机初始化
```

### 关键类和函数

#### WifiBoard 类

```cpp
class WifiBoard : public Board {
protected:
    bool wifi_config_mode_ = false;      // 配网模式标志
    void EnterWifiConfigMode();           // 进入配网模式
    virtual std::string GetBoardJson() override;

public:
    WifiBoard();
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;         // 启动网络
    virtual NetworkInterface* GetNetwork() override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveMode(bool enabled) override;
    virtual void ResetWifiConfiguration();        // 重置配网
    virtual std::string GetDeviceStatusJson() override;
};
```

#### 主要函数说明

**1. EnterWifiConfigMode()**
- **功能**: 进入 BluFi 配网模式
- **位置**: `wifi_board.cc:164-309`
- **流程**:
  1. 检查可用内存
  2. 初始化 NVS、网络接口
  3. 初始化蓝牙控制器
  4. 初始化 BluFi 服务
  5. 生成设备名称和二维码
  6. 等待配网完成
  7. 清理蓝牙资源

**2. blufi_event_callback()**
- **功能**: 处理 BluFi 事件
- **位置**: `wifi_board.cc:92-162`
- **参数**:
  - `event`: 事件类型枚举
  - `param`: 事件参数结构体

**3. blufi_dh_negotiate_data_handler()**
- **功能**: 处理 DH 密钥协商数据
- **位置**: `blufi_security.cc:66-172`
- **流程**: 接收参数 → 读取参数 → 生成公钥 → 计算共享密钥 → 派生 AES 密钥

**4. blufi_aes_encrypt() / blufi_aes_decrypt()**
- **功能**: AES 加密/解密
- **位置**: `blufi_security.cc:174-214`
- **算法**: AES-128 CFB128 模式

**5. esp_blufi_host_and_cb_init()**
- **功能**: 初始化蓝牙主机栈和回调
- **位置**: `blufi_init.cc:86-110` (Bluedroid) 或 `blufi_init.cc:262-285` (NimBLE)
- **兼容性**: 支持 Bluedroid 和 NimBLE 两种蓝牙栈

### 内存管理

**安全结构初始化:**
```cpp
esp_err_t blufi_security_init(void) {
    blufi_sec = (struct blufi_security *)malloc(sizeof(struct blufi_security));
    if (blufi_sec == NULL) {
        return ESP_FAIL;
    }
    memset(blufi_sec, 0x0, sizeof(struct blufi_security));
    mbedtls_dhm_init(&blufi_sec->dhm);
    mbedtls_aes_init(&blufi_sec->aes);
    memset(blufi_sec->iv, 0x0, sizeof(blufi_sec->iv));
    return ESP_OK;
}
```

**安全结构清理:**
```cpp
void blufi_security_deinit(void) {
    if (blufi_sec == NULL) {
        return;
    }
    if (blufi_sec->dh_param) {
        free(blufi_sec->dh_param);
        blufi_sec->dh_param = NULL;
    }
    mbedtls_dhm_free(&blufi_sec->dhm);
    mbedtls_aes_free(&blufi_sec->aes);
    memset(blufi_sec, 0x0, sizeof(struct blufi_security));  // 清零敏感数据
    free(blufi_sec);
    blufi_sec = NULL;
}
```

### 配置宏定义

```cpp
// Bluedroid 蓝牙栈
#ifdef CONFIG_BT_BLUEDROID_ENABLED
    // 使用 esp_bluedroid_* API
#endif

// NimBLE 蓝牙栈
#ifdef CONFIG_BT_NIMBLE_ENABLED
    // 使用 nimble_* API
#endif

// 蓝牙控制器
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    // 初始化蓝牙控制器
#endif
```

---

## 使用指南

### 设备端使用流程

#### 1. 自动配网 (首次启动)

设备首次启动或没有保存的 Wi-Fi 凭证时,会自动进入配网模式:

```cpp
// 检查是否有保存的 SSID
auto& ssid_manager = SsidManager::GetInstance();
auto ssid_list = ssid_manager.GetSsidList();
if (ssid_list.empty()) {
    wifi_config_mode_ = true;
    EnterWifiConfigMode();  // 自动进入配网
    return;
}
```

**设备状态指示:**
- 屏幕显示配网二维码
- 设备状态设置为 `kDeviceStateWifiConfiguring`

#### 2. 手动配网 (重置网络)

用户可以手动触发配网:

```cpp
void WifiBoard::ResetWifiConfiguration() {
    // 设置强制 AP 标志
    Settings settings("wifi", true);
    settings.SetInt("force_ap", 1);

    // 显示通知并重启
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}
```

**触发方式:**
- 通过设备菜单选择"重置网络配置"
- 长按特定按键(取决于硬件设计)

#### 3. 配网失败自动重试

如果连接已保存的 Wi-Fi 失败,自动进入配网:

```cpp
if (!wifi_station.WaitForConnected(60 * 1000)) {  // 等待 60 秒
    wifi_station.Stop();
    wifi_config_mode_ = true;
    EnterWifiConfigMode();  // 失败后自动配网
    return;
}
```

### 手机端使用流程

#### 1. 扫描设备

- 打开蓝牙
- 扫描附近的 BLE 设备
- 查找名称为 `XIAOZHI_XXXXXXXXXXXX` 的设备
- 或扫描设备屏幕上的二维码快速识别

#### 2. 连接设备

- 点击设备名称建立 BLE 连接
- 等待连接成功提示

#### 3. 输入 Wi-Fi 信息

- 输入 Wi-Fi SSID (网络名称)
- 输入 Wi-Fi 密码
- 点击"连接"按钮

#### 4. 等待配网结果

- App 显示配网进度
- 设备尝试连接 Wi-Fi
- 收到成功或失败通知

#### 5. 断开蓝牙

- 配网成功后,设备自动断开 BLE 连接
- App 可以关闭或返回主界面

### 配网二维码使用

**扫码内容示例:**
```
deviceName=XIAOZHI_A1B2C3D4E5F6&mac=A1:B2:C3:D4:E5:F6
```

**App 处理逻辑:**
1. 解析二维码字符串
2. 提取 `deviceName` 和 `mac` 字段
3. 自动搜索匹配的 BLE 设备
4. 验证 MAC 地址是否一致
5. 自动连接设备

### 配网状态监控

**设备端日志:**
```
I (12345) WifiBoard: Starting BluFi WiFi configuration
I (12350) WifiBoard: Free heap before BluFi init: 150000
I (12400) WifiBoard: Device name set to XIAOZHI_A1B2C3D4E5F6
I (12450) WifiBoard: Bluetooth MAC address: A1:B2:C3:D4:E5:F6
I (12500) WifiBoard: BLUFI VERSION 0102
I (15000) WifiBoard: Blufi event: 0  # INIT_FINISH
I (18000) WifiBoard: Blufi event: 3  # BLE_CONNECT
I (20000) WifiBoard: Recv STA SSID MyWiFi
I (21000) WifiBoard: Recv STA PASSWORD
I (22000) WifiBoard: BLUFI request wifi connect to AP
I (25000) WifiBoard: Got IP address, setting CONNECTED_BIT
I (25100) WifiBoard: BluFi configuration successful, Wi-Fi connected.
```

---

## 故障排查

### 常见问题

#### 1. 手机无法发现设备

**症状:**
- BLE 扫描列表中没有 `XIAOZHI_` 开头的设备

**可能原因:**
- 设备未进入配网模式
- 蓝牙未正确初始化
- 手机蓝牙权限未开启

**排查步骤:**
```bash
# 1. 检查设备日志
I (xxx) WifiBoard: Starting BluFi WiFi configuration  # 确认已启动

# 2. 检查蓝牙控制器状态
I (xxx) WifiBoard: BLUFI VERSION 0102  # 确认初始化成功

# 3. 检查广播状态
I (xxx) BLUFI: BLUFI init finish  # 广播应已开始

# 4. 检查内存
I (xxx) WifiBoard: Free heap before BluFi init: 150000  # 至少 100KB
```

**解决方法:**
- 重启设备进入配网模式
- 检查手机蓝牙设置和权限
- 确保设备有足够可用内存

#### 2. 连接后立即断开

**症状:**
- BLE 连接建立后几秒内断开

**可能原因:**
- 蓝牙信号干扰
- 设备内存不足
- 蓝牙栈初始化失败

**排查步骤:**
```bash
# 检查安全层初始化
I (xxx) WifiBoard: Blufi event: 3  # BLE_CONNECT
# 应该没有后续错误日志

# 检查内存分配
E (xxx) BLUFI: blufi_sec->dh_param == NULL  # 内存分配失败
E (xxx) BLUFI: malloc failed  # 内存不足
```

**解决方法:**
- 靠近设备减少信号干扰
- 重启设备释放内存
- 关闭设备上其他占用内存的功能

#### 3. DH 密钥协商失败

**症状:**
- 连接正常,但无法发送 SSID 和密码

**可能原因:**
- DH 参数传输错误
- 密钥计算失败
- mbedTLS 库错误

**排查步骤:**
```bash
# 检查 DH 参数接收
I (xxx) BLUFI: Blufi event: recv negotiate data

# 检查错误日志
E (xxx) BLUFI: read param failed -7200  # 参数读取失败
E (xxx) BLUFI: make public failed -7200  # 公钥生成失败
E (xxx) BLUFI: mbedtls_dhm_calc_secret failed -7200  # 密钥计算失败
```

**解决方法:**
- 重新连接设备
- 更新手机 App 到最新版本
- 检查 mbedTLS 库配置

#### 4. Wi-Fi 连接失败

**症状:**
- 成功发送 SSID 和密码,但无法连接 Wi-Fi

**可能原因:**
- SSID 或密码错误
- Wi-Fi 信号弱
- 路由器兼容性问题

**排查步骤:**
```bash
# 检查 SSID 接收
I (xxx) WifiBoard: Recv STA SSID MyWiFi  # 确认 SSID 正确

# 检查密码接收
I (xxx) WifiBoard: Recv STA PASSWORD  # 确认已接收(不显示内容)

# 检查连接请求
I (xxx) WifiBoard: BLUFI request wifi connect to AP

# 检查连接失败事件
I (xxx) WifiBoard: WIFI_EVENT_STA_DISCONNECTED  # 连接失败
```

**解决方法:**
- 确认 SSID 和密码输入正确
- 靠近路由器以增强信号
- 尝试 2.4GHz Wi-Fi 网络(ESP32 不支持 5GHz)
- 检查路由器安全设置 (WPA2 兼容性最佳)

#### 5. 内存不足错误

**症状:**
```
E (xxx) WifiBoard: Insufficient memory for BluFi initialization
Free heap before BluFi init: 80000
```

**可能原因:**
- 其他组件占用过多内存
- 内存碎片化
- 配置不当导致内存浪费

**解决方法:**
```cpp
// 1. 增大堆内存配置 (menuconfig)
Component config → ESP32-specific →
  Main task stack size: 4096 → 8192

// 2. 在配网前释放不必要的资源
// 例如: 停止音频服务、暂停显示更新等

// 3. 检查内存泄漏
esp_get_free_heap_size();  // 定期监控
```

### 日志分析

#### 正常配网日志流程

```
I (1000) WifiBoard: Starting BluFi WiFi configuration
I (1010) WifiBoard: Free heap before BluFi init: 150000
I (1100) WifiBoard: Device name set to XIAOZHI_A1B2C3D4E5F6
I (1120) WifiBoard: Bluetooth MAC address: A1:B2:C3:D4:E5:F6
I (1150) WifiBoard: BLUFI VERSION 0102
I (1200) WifiBoard: Free heap after BluFi init: 140000
I (3000) WifiBoard: Blufi event: 0           # ESP_BLUFI_EVENT_INIT_FINISH
I (5000) WifiBoard: Blufi event: 3           # ESP_BLUFI_EVENT_BLE_CONNECT
I (6000) WifiBoard: Recv STA SSID MyNetwork
I (6100) WifiBoard: Recv STA PASSWORD
I (6200) WifiBoard: BLUFI request wifi connect to AP
I (8000) WifiBoard: Got IP address, setting CONNECTED_BIT
I (8100) WifiBoard: BluFi configuration successful, Wi-Fi connected.
```

#### 异常日志示例

**内存不足:**
```
E (1000) WifiBoard: Insufficient memory for BluFi initialization
I (1010) WifiBoard: Free heap before BluFi init: 80000
```

**蓝牙初始化失败:**
```
E (1100) WifiBoard: initialize bt controller failed: ESP_ERR_NO_MEM
E (1110) WifiBoard: BluFi host and callback init failed: ESP_ERR_NO_MEM
```

**DH 协商失败:**
```
E (5000) BLUFI: read param failed -7200
E (5010) BLUFI: BLUFI Security is not initialized
```

**Wi-Fi 连接超时:**
```
I (6000) WifiBoard: BLUFI request wifi connect to AP
I (66000) WifiBoard: WIFI_EVENT_STA_DISCONNECTED
E (66010) WifiBoard: BluFi configuration timed out or failed.
```

### 调试建议

#### 1. 启用详细日志

```cpp
// menuconfig 配置
Component config → Log output →
  Default log verbosity: Info → Debug

// 或在代码中设置
esp_log_level_set("WifiBoard", ESP_LOG_DEBUG);
esp_log_level_set("BLUFI", ESP_LOG_DEBUG);
```

#### 2. 监控内存使用

```cpp
// 在关键位置添加内存监控
size_t free_heap = esp_get_free_heap_size();
ESP_LOGI(TAG, "Free heap: %d", free_heap);

size_t min_free_heap = esp_get_minimum_free_heap_size();
ESP_LOGI(TAG, "Minimum free heap: %d", min_free_heap);
```

#### 3. 使用 JTAG 调试

```bash
# 使用 OpenOCD 和 GDB 进行调试
openocd -f interface/ftdi/esp32_devkitj_v1.cfg -f board/esp32-wrover-kit-3.3v.cfg
xtensa-esp32-elf-gdb build/your_app.elf
```

#### 4. 分析 Wi-Fi 连接问题

```cpp
// 获取详细的 Wi-Fi 断开原因
void wifi_event_handler(void* arg, esp_event_base_t event_base,
                       int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconnected =
            (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGE(TAG, "Disconnect reason: %d", disconnected->reason);
        // 0: 未指定
        // 2: 认证超时
        // 15: 4-Way Handshake 超时
        // 201: 密码错误
    }
}
```

### 性能优化建议

#### 1. 减少配网时间

```cpp
// 优化 Wi-Fi 连接超时
wifi_station.WaitForConnected(30 * 1000);  // 从 60s 减到 30s

// 提高 Wi-Fi 扫描速度
wifi_scan_config_t scan_config = {
    .scan_time.active.min = 100,  // 默认 120ms
    .scan_time.active.max = 300,  // 默认 600ms
};
```

#### 2. 优化蓝牙性能

```cpp
// 调整 BLE 连接参数
esp_ble_conn_update_params_t conn_params = {
    .min_int = 0x10,    // 20ms
    .max_int = 0x20,    // 40ms
    .latency = 0,
    .timeout = 400,     // 4s
};
```

#### 3. 减少内存占用

```cpp
// 配网完成后立即释放蓝牙资源
esp_blufi_host_deinit();
esp_bt_controller_disable();
esp_bt_controller_deinit();

// ESP32 特定优化
#if CONFIG_IDF_TARGET_ESP32
esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
#endif
```

---

## 附录

### A. 相关 API 参考

**ESP-IDF BluFi API:**
- [BluFi 编程指南](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/blufi.html)
- [BluFi API 参考](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/bluetooth/esp_blufi.html)

**mbedTLS 加密库:**
- [mbedTLS DH 模块](https://github.com/ARMmbed/mbedtls/blob/development/library/dhm.c)
- [mbedTLS AES 模块](https://github.com/ARMmbed/mbedtls/blob/development/library/aes.c)

### B. 配置选项

**sdkconfig 相关配置:**
```
CONFIG_BT_ENABLED=y
CONFIG_BT_BLUEDROID_ENABLED=y      # 或 CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_CONTROLLER_ENABLED=y
CONFIG_BLUEDROID_ENABLED=y

# 蓝牙内存配置
CONFIG_BTDM_CTRL_BLE_MAX_CONN=3
CONFIG_BTDM_CTRL_BLE_MAX_CONN_EFF=3

# Wi-Fi 配置
CONFIG_ESP32_WIFI_STATIC_RX_BUFFER_NUM=10
CONFIG_ESP32_WIFI_DYNAMIC_RX_BUFFER_NUM=32
CONFIG_ESP32_WIFI_DYNAMIC_TX_BUFFER_NUM=32
```

### C. 开发者注意事项

1. **线程安全**: BluFi 回调函数在蓝牙任务上下文中执行,访问共享资源需加锁
2. **内存管理**: 及时释放 DH 参数缓冲区,避免内存泄漏
3. **错误处理**: 所有 ESP-IDF API 调用都应检查返回值
4. **安全考虑**: 生产环境建议实现额外的设备认证机制
5. **兼容性**: 测试不同手机型号和 Android/iOS 版本的兼容性

### D. 版本历史

- **v1.0**: 初始实现,支持基本 BluFi 配网
- **v1.1**: 添加二维码支持,优化内存使用
- **v1.2**: 支持 NimBLE 蓝牙栈,改进错误处理
- **当前版本**: 完整的安全加密和状态报告

---

## 总结

本文档详细介绍了基于 ESP32 的 BluFi 蓝牙配网实现,涵盖了从原理到实践的各个方面:

- **配网流程**: 从设备初始化到 Wi-Fi 连接的完整流程
- **安全机制**: DH 密钥交换、AES 加密、CRC 校验的详细说明
- **协议详解**: BluFi 协议层次、事件处理、数据格式
- **代码实现**: 核心代码结构和关键函数解析
- **使用指南**: 设备端和手机端的操作步骤
- **故障排查**: 常见问题、日志分析、调试建议

通过本文档,开发者可以全面了解 BluFi 配网技术,并能够有效地集成、调试和优化蓝牙配网功能。
