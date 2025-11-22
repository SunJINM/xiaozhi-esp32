# MCP 服务端消息格式参考文档

本文档详细说明 ESP32 设备（MCP 服务器）发送给后台 API（MCP 客户端）的消息格式规范。

> **相关文档**：[mcp-protocol.md](./mcp-protocol.md) - MCP 协议交互流程概述

---

## 快速参考

| 消息类型 | 方法 | 发送方 | 说明 |
|---------|------|--------|------|
| 初始化响应 | `initialize` | 设备 | 返回设备信息和协议版本 |
| 工具列表响应 | `tools/list` | 设备 | 返回可用工具列表（支持分页） |
| 工具调用响应 | `tools/call` | 设备 | 返回工具执行结果或错误 |
| 错误响应 | 任意 | 设备 | 通用错误响应格式 |

---

## 一、响应消息基础格式

所有响应消息遵循 JSON-RPC 2.0 规范，封装在 `payload` 字段中。

### 1.1 成功响应（Success Response）

```json
{
  "jsonrpc": "2.0",
  "id": <request_id>,
  "result": {
    // 具体结果内容
  }
}
```

### 1.2 错误响应（Error Response）

```json
{
  "jsonrpc": "2.0",
  "id": <request_id>,
  "error": {
    "message": "错误描述"
  }
}
```

**常见错误消息**：
- `"Missing params"` - 缺少必需参数
- `"Missing name"` - 缺少工具名称
- `"Invalid arguments"` - 参数格式错误
- `"Unknown tool: <tool_name>"` - 工具不存在
- `"Missing valid argument: <arg_name>"` - 缺少必需参数值

> **代码位置**：[mcp_server.cc:337-344](../main/mcp_server.cc#L337)

---

## 二、初始化响应 (initialize)

### 2.1 请求格式（客户端发送）

```json
{
  "jsonrpc": "2.0",
  "method": "initialize",
  "params": {
    "capabilities": {
      "vision": {
        "url": "http://api.example.com/vision",
        "token": "your_token_here"
      }
    }
  },
  "id": 1
}
```

### 2.2 响应格式（设备返回）

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "protocolVersion": "2024-11-05",
    "capabilities": {
      "tools": {}
    },
    "serverInfo": {
      "name": "XIAOZHI_ESP32S3",
      "version": "1.0.0"
    }
  }
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `protocolVersion` | string | MCP 协议版本号 |
| `serverInfo.name` | string | 设备名称（`BOARD_NAME`） |
| `serverInfo.version` | string | 固件版本号 |

> **代码位置**：[mcp_server.cc:277-288](../main/mcp_server.cc#L277)

---

## 三、工具列表响应 (tools/list)

### 3.1 请求格式（客户端发送）

```json
{
  "jsonrpc": "2.0",
  "method": "tools/list",
  "params": {
    "cursor": ""
  },
  "id": 2
}
```

**分页参数**：
- `cursor`: 空字符串表示首次请求，非空值用于获取下一页

### 3.2 响应格式（设备返回）

```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "result": {
    "tools": [
      {
        "name": "self.get_device_status",
        "description": "Provides the real-time information of the device...",
        "inputSchema": {
          "type": "object",
          "properties": {},
          "required": []
        }
      },
      {
        "name": "self.audio_speaker.set_volume",
        "description": "Set the volume of the audio speaker...",
        "inputSchema": {
          "type": "object",
          "properties": {
            "volume": {
              "type": "integer",
              "minimum": 0,
              "maximum": 100
            }
          },
          "required": ["volume"]
        }
      }
    ],
    "nextCursor": "self.screen.set_brightness"
  }
}
```

**响应字段说明**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `tools` | array | 工具对象数组 |
| `tools[].name` | string | 工具唯一标识名 |
| `tools[].description` | string | 工具功能描述 |
| `tools[].inputSchema` | object | JSON Schema 格式的参数定义 |
| `nextCursor` | string | 下一页游标，空表示无更多数据 |

**分页机制**：
- 单次响应最大 8000 字符
- 如果工具列表超出限制，`nextCursor` 返回下一个工具名称
- 客户端使用 `nextCursor` 值继续请求

> **代码位置**：[mcp_server.cc:346-395](../main/mcp_server.cc#L346)

---

## 四、工具调用响应 (tools/call)

### 4.1 请求格式（客户端发送）

```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "self.audio_speaker.set_volume",
    "arguments": {
      "volume": 75
    },
    "stackSize": 6144
  },
  "id": 3
}
```

**参数说明**：

| 字段 | 类型 | 必需 | 说明 |
|------|------|------|------|
| `name` | string | ✅ | 要调用的工具名称 |
| `arguments` | object | ✅ | 工具参数（可为空对象） |
| `stackSize` | integer | ❌ | 线程栈大小（默认 6144） |

### 4.2 响应格式（设备返回）

#### 成功响应

不同返回类型的示例：

**返回布尔值**：
```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "true"
      }
    ],
    "isError": false
  }
}
```

**返回 JSON 对象**：
```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "{\"audio_speaker\":{\"volume\":75},\"screen\":{\"brightness\":80}}"
      }
    ],
    "isError": false
  }
}
```

#### 失败响应

```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "error": {
    "message": "Unknown tool: self.non_existent_tool"
  }
}
```

> **代码位置**：[mcp_server.cc:397-456](../main/mcp_server.cc#L397)

---

## 五、系统工具详细说明

### 5.1 self.get_device_status

**功能**：查询设备实时状态信息

**参数**：无

**返回值示例**：
```json
{
  "audio_speaker": {
    "volume": 70
  },
  "screen": {
    "brightness": 100,
    "theme": "light"
  },
  "battery": {
    "level": 85,
    "charging": true
  },
  "network": {
    "type": "wifi",
    "ssid": "MyWiFi",
    "signal": "strong"
  },
  "chip": {
    "temperature": 45
  }
}
```

**完整调用示例**：

请求：
```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "self.get_device_status",
    "arguments": {}
  },
  "id": 10
}
```

响应：
```json
{
  "jsonrpc": "2.0",
  "id": 10,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "{\"audio_speaker\":{\"volume\":70},\"screen\":{\"brightness\":100,\"theme\":\"light\"},\"battery\":{\"level\":85,\"charging\":true},\"network\":{\"type\":\"wifi\",\"ssid\":\"MyWiFi\",\"signal\":\"strong\"},\"chip\":{\"temperature\":45}}"
      }
    ],
    "isError": false
  }
}
```

> **代码位置**：[mcp_server.cc:43-51](../main/mcp_server.cc#L43)

---

### 5.2 self.audio_speaker.set_volume

**功能**：设置音频扬声器音量

**参数**：

| 参数名 | 类型 | 范围 | 必需 | 说明 |
|--------|------|------|------|------|
| `volume` | integer | 0-100 | ✅ | 目标音量值 |

**返回值**：`true`（布尔值）

**完整调用示例**：

请求：
```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "self.audio_speaker.set_volume",
    "arguments": {
      "volume": 50
    }
  },
  "id": 11
}
```

响应：
```json
{
  "jsonrpc": "2.0",
  "id": 11,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "true"
      }
    ],
    "isError": false
  }
}
```

> **代码位置**：[mcp_server.cc:53-62](../main/mcp_server.cc#L53)

---

### 5.3 self.screen.set_brightness

**功能**：设置屏幕亮度

**参数**：

| 参数名 | 类型 | 范围 | 必需 | 说明 |
|--------|------|------|------|------|
| `brightness` | integer | 0-100 | ✅ | 目标亮度值 |

**返回值**：`true`（布尔值）

**完整调用示例**：

请求：
```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "self.screen.set_brightness",
    "arguments": {
      "brightness": 80
    }
  },
  "id": 12
}
```

响应：
```json
{
  "jsonrpc": "2.0",
  "id": 12,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "true"
      }
    ],
    "isError": false
  }
}
```

> **代码位置**：[mcp_server.cc:66-76](../main/mcp_server.cc#L66)

---

### 5.4 self.screen.set_theme

**功能**：设置屏幕主题

**参数**：

| 参数名 | 类型 | 可选值 | 必需 | 说明 |
|--------|------|--------|------|------|
| `theme` | string | `"light"`, `"dark"` | ✅ | 目标主题 |

**返回值**：`true`（布尔值）

**完整调用示例**：

请求：
```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "self.screen.set_theme",
    "arguments": {
      "theme": "dark"
    }
  },
  "id": 13
}
```

响应：
```json
{
  "jsonrpc": "2.0",
  "id": 13,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "true"
      }
    ],
    "isError": false
  }
}
```

> **代码位置**：[mcp_server.cc:80-89](../main/mcp_server.cc#L80)

---

### 5.5 self.camera.take_photo

**功能**：拍照并使用视觉模型分析照片内容

**参数**：

| 参数名 | 类型 | 必需 | 说明 |
|--------|------|------|------|
| `question` | string | ✅ | 关于照片的问题 |

**返回值**：JSON 对象，包含拍照结果和分析内容

**成功返回示例**：
```json
{
  "success": true,
  "analysis": "照片中显示了一个红色的杯子放在木质桌面上..."
}
```

**失败返回示例**：
```json
{
  "success": false,
  "message": "Failed to capture photo"
}
```

**完整调用示例**：

请求：
```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "self.camera.take_photo",
    "arguments": {
      "question": "What objects do you see in this photo?"
    }
  },
  "id": 14
}
```

响应：
```json
{
  "jsonrpc": "2.0",
  "id": 14,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "{\"success\":true,\"analysis\":\"I can see a red mug on a wooden table...\"}"
      }
    ],
    "isError": false
  }
}
```

> **代码位置**：[mcp_server.cc:93-109](../main/mcp_server.cc#L93)

---

## 六、完整交互示例

### 场景：查询设备状态并调整音量

**步骤 1：初始化会话**

客户端请求：
```json
{
  "jsonrpc": "2.0",
  "method": "initialize",
  "params": {
    "capabilities": {}
  },
  "id": 1
}
```

设备响应：
```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "protocolVersion": "2024-11-05",
    "capabilities": {"tools": {}},
    "serverInfo": {
      "name": "XIAOZHI_ESP32S3",
      "version": "1.0.0"
    }
  }
}
```

---

**步骤 2：获取工具列表**

客户端请求：
```json
{
  "jsonrpc": "2.0",
  "method": "tools/list",
  "params": {
    "cursor": ""
  },
  "id": 2
}
```

设备响应：
```json
{
  "jsonrpc": "2.0",
  "id": 2,
  "result": {
    "tools": [
      {
        "name": "self.get_device_status",
        "description": "Provides the real-time information of the device...",
        "inputSchema": {"type": "object", "properties": {}, "required": []}
      },
      {
        "name": "self.audio_speaker.set_volume",
        "description": "Set the volume of the audio speaker...",
        "inputSchema": {
          "type": "object",
          "properties": {
            "volume": {"type": "integer", "minimum": 0, "maximum": 100}
          },
          "required": ["volume"]
        }
      }
    ],
    "nextCursor": ""
  }
}
```

---

**步骤 3：查询设备当前状态**

客户端请求：
```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "self.get_device_status",
    "arguments": {}
  },
  "id": 3
}
```

设备响应：
```json
{
  "jsonrpc": "2.0",
  "id": 3,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "{\"audio_speaker\":{\"volume\":30},\"screen\":{\"brightness\":100}}"
      }
    ],
    "isError": false
  }
}
```

---

**步骤 4：调整音量到 70**

客户端请求：
```json
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "self.audio_speaker.set_volume",
    "arguments": {
      "volume": 70
    }
  },
  "id": 4
}
```

设备响应：
```json
{
  "jsonrpc": "2.0",
  "id": 4,
  "result": {
    "content": [
      {
        "type": "text",
        "text": "true"
      }
    ],
    "isError": false
  }
}
```

---

## 七、蓝牙配网消息格式（BluFi）

蓝牙配网使用 `esp_blufi_send_custom_data()` 发送 JSON 消息。

### 7.1 配网成功消息

```json
{
  "status": 0,
  "mac": "AA:BB:CC:DD:EE:FF",
  "ssid": "YourWiFiName"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `status` | integer | 状态码，0 表示成功 |
| `mac` | string | WiFi MAC 地址 |
| `ssid` | string | 已连接的 WiFi 名称 |

---

### 7.2 配网失败消息

```json
{
  "status": 1,
  "msg": "密码错误"
}
```

**状态码定义**：

| status | 含义 | msg 示例 |
|--------|------|----------|
| `0` | 成功 | - |
| `1` | 密码错误 | "密码错误" |
| `2` | WiFi未找到 | "WiFi未找到" |
| `3` | 连接超时 | "连接超时" |
| `4` | 未知错误 | "未知错误" |

**失败原因映射**（基于 ESP32 WiFi 事件）：

| ESP32 事件 | status | 说明 |
|-----------|--------|------|
| `WIFI_REASON_AUTH_FAIL` | 1 | 认证失败 |
| `WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT` | 1 | 四次握手超时 |
| `WIFI_REASON_HANDSHAKE_TIMEOUT` | 1 | 握手超时 |
| `WIFI_REASON_NO_AP_FOUND` | 2 | AP 未找到 |
| `WIFI_REASON_BEACON_TIMEOUT` | 2 | 信标超时 |
| `WIFI_REASON_ASSOC_EXPIRE` | 3 | 关联过期 |
| `WIFI_REASON_CONNECTION_FAIL` | 3 | 连接失败 |
| 其他原因 | 4 | 未分类错误 |

> **代码位置**：
> - 成功消息：[wifi_board.cc:299-309](../main/boards/common/wifi_board.cc#L299)
> - 失败消息：[wifi_board.cc:316-340](../main/boards/common/wifi_board.cc#L316)
> - 事件处理：[wifi_board.cc:70-91](../main/boards/common/wifi_board.cc#L70)

---

## 八、实现细节参考

### 8.1 消息构建

**成功响应构建**（C++ 代码）：
```cpp
void McpServer::ReplyResult(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}
```

**错误响应构建**（C++ 代码）：
```cpp
void McpServer::ReplyError(int id, const std::string& message) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}
```

> **代码位置**：[mcp_server.cc:329-344](../main/mcp_server.cc#L329)

---

### 8.2 参数类型支持

MCP 服务器支持以下参数类型：

| 类型 | C++ 枚举 | JSON Schema 类型 | 说明 |
|------|----------|------------------|------|
| 布尔值 | `kPropertyTypeBoolean` | `boolean` | true/false |
| 整数 | `kPropertyTypeInteger` | `integer` | 支持范围限制 |
| 字符串 | `kPropertyTypeString` | `string` | 任意字符串 |

**参数定义示例**（C++ 代码）：
```cpp
PropertyList({
    Property("volume", kPropertyTypeInteger, 0, 100),  // 整数，范围 0-100
    Property("theme", kPropertyTypeString),            // 字符串，无默认值
    Property("enabled", kPropertyTypeBoolean, true)    // 布尔值，默认 true
})
```

---

## 九、注意事项

### 9.1 消息大小限制

- **工具列表响应**：单次最大 8000 字符
- **超出处理**：自动分页，通过 `nextCursor` 字段指示

### 9.2 线程安全

- 工具调用在独立线程中执行（默认栈大小 6144 字节）
- 客户端可通过 `stackSize` 参数自定义栈大小

### 9.3 错误处理

- 所有工具调用应捕获异常并返回错误响应
- 参数验证在调用前完成
- 缺少必需参数时立即返回错误

### 9.4 通信协议

MCP 消息封装在基础传输协议中：
- **WebSocket**：实时双向通信
- **MQTT**：发布/订阅模式

完整消息结构：
```json
{
  "session_id": "xxx",
  "type": "mcp",
  "payload": {
    // JSON-RPC 2.0 消息
  }
}
```

---

## 附录：相关文档

- [MCP 协议交互流程](./mcp-protocol.md)
- [MCP 使用说明](./mcp-usage.md)
- [蓝牙配网说明](./BluFi蓝牙配网说明文档.md)
- [WebSocket 协议](./websocket.md)
- [MQTT/UDP 协议](./mqtt-udp.md)

---

**文档版本**：1.0
**最后更新**：2025-11-21
**维护者**：Xiaozhi Team
