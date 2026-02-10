#include <WiFi.h>
#include <ArduinoJson.h>
#include <WebSocketMCP.h>
#include <Adafruit_NeoPixel.h>

// === 新增：溫度傳感器庫 ===
#include <OneWire.h>
#include <DallasTemperature.h>

// === 硬體定義 ===
#define LED_BUILTIN 18      // 板載 LED
#define WS2812_PIN 40       // WS2812 數據腳位
#define NUM_LEDS 5          // WS2812 LED 數量
#define ONE_WIRE_BUS 48     // [新增] DS18B20 數據腳位

// === WiFi 配置 ===
const char* ssid = "louisguan";
const char* password = "0989839679";

// === MCP 服務器配置 ===
const char* mcpEndpoint = "wss://api.xiaozhi.me/mcp/?token=eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9.eyJ1c2VySWQiOjQwNzc5MywiYWdlbnRJZCI6MTI0NDc0NSwiZW5kcG9pbnRJZCI6ImFnZW50XzEyNDQ3NDUiLCJwdXJwb3NlIjoibWNwLWVuZHBvaW50IiwiaWF0IjoxNzY2Njg0NjU2LCJleHAiOjE3OTgyNDIyNTZ9.Z-N1t69UlzPUWPnXuRzdPDlW4EMSElvMPSSio6ee-C7UsApBD4z2OdnN6PE5qkc0pk2HVs7titjqCxnyCvTVvg";

// === 全局物件 ===
WebSocketMCP mcpClient;

// 建立 NeoPixel 物件
Adafruit_NeoPixel strip(NUM_LEDS, WS2812_PIN, NEO_GRB + NEO_KHZ800);

// [新增] 建立溫度傳感器物件
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// === 燈光特效狀態管理 ===
enum LedEffect {
  EFFECT_NONE,
  EFFECT_RAINBOW,
  EFFECT_THEATER_RAINBOW,
  EFFECT_COLOR_WIPE
};

LedEffect currentEffect = EFFECT_NONE;
unsigned long lastEffectUpdate = 0;
int effectStep = 0;
uint32_t wipeColor = 0;

// --- 函數前向宣告 ---
void registerMcpTools();
void onConnectionStatus(bool connected);
void handleLedEffects();
uint32_t Wheel(byte WheelPos);

// === 連接狀態回調 ===
void onConnectionStatus(bool connected) {
  if (connected) {
    Serial.println("[MCP] 已連接到服務器");
    registerMcpTools();
  } else {
    Serial.println("[MCP] 與服務器斷開連接");
  }
}

// === 註冊 MCP 工具 ===
void registerMcpTools() {

  // 1. 原有的板載 LED 工具
  mcpClient.registerTool(
    "led_blink",
    "控制ESP32板載LED (開/關/閃爍)",
    "{\"type\":\"object\",\"properties\":{\"state\":{\"type\":\"string\",\"enum\":[\"on\",\"off\",\"blink\"]}},\"required\":[\"state\"]}",
  [](const String & args) {
    DynamicJsonDocument doc(256);
    deserializeJson(doc, args);
    String state = doc["state"].as<String>();

    if (state == "on") digitalWrite(LED_BUILTIN, HIGH);
    else if (state == "off") digitalWrite(LED_BUILTIN, LOW);
    else if (state == "blink") {
      for (int i = 0; i < 3; i++) {
        digitalWrite(LED_BUILTIN, HIGH); delay(200);
        digitalWrite(LED_BUILTIN, LOW); delay(200);
      }
    }
    return WebSocketMCP::ToolResponse("{\"success\":true,\"state\":\"" + state + "\"}");
  }
  );

  // 2. WS2812B 控制工具
  const char* ws2812Schema =
    "{"
    "\"type\": \"object\","
    "\"properties\": {"
    "\"mode\": { \"type\": \"string\", \"enum\": [\"rainbow\", \"theater\", \"wipe\", \"single\", \"clear\"] },"
    "\"index\": { \"type\": \"integer\", \"description\": \"LED index 0-4 (only for mode='single')\" },"
    "\"r\": { \"type\": \"integer\", \"description\": \"Red 0-255\" },"
    "\"g\": { \"type\": \"integer\", \"description\": \"Green 0-255\" },"
    "\"b\": { \"type\": \"integer\", \"description\": \"Blue 0-255\" }"
    "},"
    "\"required\": [\"mode\"]"
    "}";

  mcpClient.registerTool(
    "ws2812_control",
    "控制WS2812燈條特效或指定單顆燈顏色。Mode可用: rainbow(彩虹), theater(跑馬燈), wipe(單色刷過), single(指定單顆), clear(關閉)",
    ws2812Schema,
  [](const String & args) {
    DynamicJsonDocument doc(512);
    deserializeJson(doc, args);

    String mode = doc["mode"].as<String>();
    int r = doc["r"] | 0;
    int g = doc["g"] | 0;
    int b = doc["b"] | 0;

    String responseMsg = "Mode set to " + mode;

    if (mode == "rainbow") {
      currentEffect = EFFECT_RAINBOW;
      effectStep = 0;
    }
    else if (mode == "theater") {
      currentEffect = EFFECT_THEATER_RAINBOW;
      effectStep = 0;
    }
    else if (mode == "wipe") {
      currentEffect = EFFECT_COLOR_WIPE;
      wipeColor = strip.Color(r, g, b);
      effectStep = 0;
    }
    else if (mode == "clear") {
      currentEffect = EFFECT_NONE;
      strip.clear();
      strip.show();
    }
    else if (mode == "single") {
      currentEffect = EFFECT_NONE;
      int idx = doc["index"];
      if (idx >= 0 && idx < NUM_LEDS) {
        strip.setPixelColor(idx, strip.Color(r, g, b));
        strip.show();
        responseMsg += " LED " + String(idx) + " set to RGB(" + r + "," + g + "," + b + ")";
      } else {
        return WebSocketMCP::ToolResponse("{\"success\":false,\"error\":\"Index out of range\"}");
      }
    }

    return WebSocketMCP::ToolResponse("{\"success\":true,\"msg\":\"" + responseMsg + "\"}");
  }
  );

  // 3. [新增] 讀取溫度工具
  // 這個工具沒有參數，AI 呼叫後會返回攝氏溫度
  mcpClient.registerTool(
    "read_temperature",
    "讀取 DS18B20 傳感器的環境溫度 (攝氏)",
    "{}", // 不需要參數
  [](const String & args) {
    Serial.println("Requesting temperature...");

    // 發送讀取命令 (預設會阻塞約 750ms 等待轉換)
    sensors.requestTemperatures();
    float tempC = sensors.getTempCByIndex(0);

    Serial.print("Temperature is: ");
    Serial.println(tempC);

    // 檢查是否讀取失敗 (-127 代表斷線)
    if (tempC == DEVICE_DISCONNECTED_C) {
      return WebSocketMCP::ToolResponse("{\"success\":false, \"error\":\"Sensor disconnected\"}");
    }

    return WebSocketMCP::ToolResponse("{\"success\":true, \"temperature_c\":" + String(tempC) + "}");
  }
  );

  Serial.println("[MCP] 所有工具已註冊 (含溫度)");
}

// === 特效處理邏輯 (非阻塞) ===
void handleLedEffects() {
  unsigned long currentMillis = millis();

  switch (currentEffect) {
    case EFFECT_RAINBOW:
      if (currentMillis - lastEffectUpdate > 20) {
        lastEffectUpdate = currentMillis;
        for (int i = 0; i < strip.numPixels(); i++) {
          strip.setPixelColor(i, Wheel((i * 256 / strip.numPixels() + effectStep) & 255));
        }
        strip.show();
        effectStep++;
        if (effectStep >= 256) effectStep = 0;
      }
      break;

    case EFFECT_THEATER_RAINBOW:
      if (currentMillis - lastEffectUpdate > 80) {
        lastEffectUpdate = currentMillis;
        for (int i = 0; i < strip.numPixels(); i = i + 3) {
          strip.setPixelColor(i + ((effectStep + 2) % 3), 0);
        }
        int q = effectStep % 3;
        int cycle = effectStep / 3;
        for (int i = 0; i < strip.numPixels(); i = i + 3) {
          if (i + q < strip.numPixels()) {
            strip.setPixelColor(i + q, Wheel((i + cycle) % 255));
          }
        }
        strip.show();
        effectStep++;
        if (effectStep >= 256 * 3) effectStep = 0;
      }
      break;

    case EFFECT_COLOR_WIPE:
      if (currentMillis - lastEffectUpdate > 50) {
        lastEffectUpdate = currentMillis;
        if (effectStep < strip.numPixels()) {
          strip.setPixelColor(effectStep, wipeColor);
          strip.show();
          effectStep++;
        }
      }
      break;

    case EFFECT_NONE:
    default:
      break;
  }
}

// 輔助函數：產生彩虹顏色
uint32_t Wheel(byte WheelPos) {
  WheelPos = 255 - WheelPos;
  if (WheelPos < 85) {
    return strip.Color(255 - WheelPos * 3, 0, WheelPos * 3);
  }
  if (WheelPos < 170) {
    WheelPos -= 85;
    return strip.Color(0, WheelPos * 3, 255 - WheelPos * 3);
  }
  WheelPos -= 170;
  return strip.Color(WheelPos * 3, 255 - WheelPos * 3, 0);
}

void setup() {
  Serial.begin(115200);

  // 初始化板載 LED
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // 初始化 WS2812
  strip.begin();
  strip.show();
  strip.setBrightness(100);
  Serial.println("WS2812 Initialized");

  // [新增] 初始化溫度傳感器
  sensors.begin();
  Serial.println("DS18B20 Initialized on Pin " + String(ONE_WIRE_BUS));

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

  // 初始化 MCP
  mcpClient.begin(mcpEndpoint, onConnectionStatus);
}

void loop() {
  mcpClient.loop();
  handleLedEffects();
  delay(5);
}
