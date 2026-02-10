#include <WiFi.h>
// 必須添加 ArduinoJson 庫來處理 JSON 數據 (請在庫管理器安裝 ArduinoJson v6)
#include <ArduinoJson.h> 
#include <WebSocketMCP.h>

// 您原本設定的 GPIO 18
#define LED_BUILTIN 18 

// === 您的 WiFi 配置 ===
const char* ssid = "louisguan";
const char* password = "0989839679";

// === 您的 MCP 服務器配置 ===
const char* mcpEndpoint = "wss://api.xiaozhi.me/mcp/?token=eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9.eyJ1c2VySWQiOjQwNzc5MywiYWdlbnRJZCI6MTI0NDc0NSwiZW5kcG9pbnRJZCI6ImFnZW50XzEyNDQ3NDUiLCJwdXJwb3NlIjoibWNwLWVuZHBvaW50IiwiaWF0IjoxNzY2Njg0NjU2LCJleHAiOjE3OTgyNDIyNTZ9.Z-N1t69UlzPUWPnXuRzdPDlW4EMSElvMPSSio6ee-C7UsApBD4z2OdnN6PE5qkc0pk2HVs7titjqCxnyCvTVvg";

// 創建 WebSocketMCP 實例
WebSocketMCP mcpClient;

// --- 函數前向宣告 (解決編譯順序問題) ---
void registerMcpTools();
void onConnectionStatus(bool connected);

// 連接狀態回調函數
void onConnectionStatus(bool connected) {
  if (connected) {
    Serial.println("[MCP] 已連接到服務器");
    // 連接成功後註冊工具
    registerMcpTools();
  } else {
    Serial.println("[MCP] 與服務器斷開連接");
  }
}

// 註冊 MCP 工具
void registerMcpTools() {
  // 註冊一個簡單的 LED 控制工具
  mcpClient.registerTool(
    "led_blink",
    "控制ESP32板載LED",
    "{\"type\":\"object\",\"properties\":{\"state\":{\"type\":\"string\",\"enum\":[\"on\",\"off\",\"blink\"]}},\"required\":[\"state\"]}",
    [](const String& args) {
      // 使用 DynamicJsonDocument 解析參數
      DynamicJsonDocument doc(512);
      DeserializationError error = deserializeJson(doc, args);

      if (error) {
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.f_str());
        return WebSocketMCP::ToolResponse("{\"success\":false,\"error\":\"Invalid JSON\"}");
      }

      String state = doc["state"].as<String>();
      Serial.println("收到指令: " + state);
      
      if (state == "on") {
        digitalWrite(LED_BUILTIN, HIGH);
      } else if (state == "off") {
        digitalWrite(LED_BUILTIN, LOW);
      } else if (state == "blink") {
        for (int i = 0; i < 3; i++) { // 閃爍 3 次
          digitalWrite(LED_BUILTIN, HIGH);
          delay(200);
          digitalWrite(LED_BUILTIN, LOW);
          delay(200);
        }
      }
      
      return WebSocketMCP::ToolResponse("{\"success\":true,\"state\":\"" + state + "\"}");
    }
  );
  Serial.println("[MCP] LED控制工具已註冊");
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // 連接 WiFi
  Serial.print("連接到 WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\nWiFi 已連接");
  Serial.println("IP地址: " + WiFi.localIP().toString());

  // 初始化 MCP 客戶端
  mcpClient.begin(mcpEndpoint, onConnectionStatus);
}

void loop() {
  // 處理 MCP 客戶端事件
  mcpClient.loop();
  
  delay(10);
}
