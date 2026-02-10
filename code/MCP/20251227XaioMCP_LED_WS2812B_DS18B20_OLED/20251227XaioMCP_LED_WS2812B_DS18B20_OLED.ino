/*
   專案: ESP32-S3 小智 MCP 控制器 (OneWireNg + DallasTemperature 非阻塞版)
   功能: 語音控制 LED、OLED 顯示狀態、背景持續讀取溫度
 * * [庫需求] 請在庫管理員安裝:
   1. OneWireNg (建議移除舊的 "OneWire" 庫以避免衝突，或確保編譯器抓到的是 OneWireNg)
   2. DallasTemperature
   3. Adafruit NeoPixel
   4. U8g2
   5. ArduinoJson, WebSocketMCP...
*/

#include <WiFi.h>
#include <ArduinoJson.h>
#include <WebSocketMCP.h>
#include <Adafruit_NeoPixel.h>

// === 1. 溫度相關庫 ===
// 這裡使用標準 include，OneWireNg 會模擬標準 OneWire 介面
// 請確保您的開發環境中，OneWire.h 是來自 OneWireNg 庫
#include <OneWire.h>
#include <DallasTemperature.h>

// === 2. OLED 顯示庫 ===
#include <Arduino.h>
#include <U8g2lib.h>

#ifdef U8X8_HAVE_HW_SPI
#include <SPI.h>
#endif
#ifdef U8X8_HAVE_HW_I2C
#include <Wire.h>
#endif

// [OLED 設定] 128x32, Page Buffer 模式 (省記憶體)
U8G2_SSD1306_128X32_UNIVISION_1_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// === 硬體腳位定義 ===
#define LED_BUILTIN 18
#define WS2812_PIN 40
#define NUM_LEDS 5
#define ONE_WIRE_BUS 48     // DS18B20 腳位
#define I2C_SDA_PIN 41      // OLED SDA
#define I2C_SCL_PIN 42      // OLED SCL

// === WiFi 配置 ===
const char* ssid = "louisguan";
const char* password = "0989839679";

// === MCP 服務器配置 ===
const char* mcpEndpoint = "wss://api.xiaozhi.me/mcp/?token=eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9.eyJ1c2VySWQiOjQwNzc5MywiYWdlbnRJZCI6MTI0NDc0NSwiZW5kcG9pbnRJZCI6ImFnZW50XzEyNDQ3NDUiLCJwdXJwb3NlIjoibWNwLWVuZHBvaW50IiwiaWF0IjoxNzY2Njg0NjU2LCJleHAiOjE3OTgyNDIyNTZ9.Z-N1t69UlzPUWPnXuRzdPDlW4EMSElvMPSSio6ee-C7UsApBD4z2OdnN6PE5qkc0pk2HVs7titjqCxnyCvTVvg";

// === 全局物件 ===
WebSocketMCP mcpClient;
Adafruit_NeoPixel strip(NUM_LEDS, WS2812_PIN, NEO_GRB + NEO_KHZ800);

// [溫度物件初始化]
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// === 狀態變數 ===
String connectionStatus = "Connecting...";
String currentModeName = "None";
float cachedTemp = -127.0; // [關鍵] 儲存最新的溫度值 (背景讀取用)
unsigned long lastDisplayUpdate = 0;

// === 燈光特效狀態 ===
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
void handleDisplay();
void handleTemperature(); // [新增] 背景溫度處理
uint32_t Wheel(byte WheelPos);
void drawOledContent();

// === 連接狀態回調 ===
void onConnectionStatus(bool connected) {
  if (connected) {
    Serial.println("[MCP] 已連接到服務器");
    connectionStatus = "MCP: OK";
    registerMcpTools();
  } else {
    Serial.println("[MCP] 與服務器斷開連接");
    connectionStatus = "MCP: Disconnect";
  }
}

// === 註冊 MCP 工具 ===
void registerMcpTools() {
  // 1. 板載 LED 工具
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
    currentModeName = mode;

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
        responseMsg += " LED " + String(idx);
      } else {
        return WebSocketMCP::ToolResponse("{\"success\":false,\"error\":\"Index out of range\"}");
      }
    }

    return WebSocketMCP::ToolResponse("{\"success\":true,\"msg\":\"" + responseMsg + "\"}");
  }
  );

  // 3. 讀取溫度工具 (現在直接回傳背景讀取的數值)
  mcpClient.registerTool(
    "read_temperature",
    "讀取 DS18B20 傳感器的環境溫度 (攝氏)",
    "{}",
  [](const String & args) {
    Serial.println("AI Requesting temperature...");
    // [優化] 直接回傳變數，不進行 I/O 操作，速度最快
    float tempC = cachedTemp;

    Serial.print("Returning Cached Temp: ");
    Serial.println(tempC);

    // 如果尚未讀取到有效數值 (初始狀態)
    if (tempC <= -100.0) {
      return WebSocketMCP::ToolResponse("{\"success\":false, \"error\":\"Sensor initializing...\"}");
    }

    return WebSocketMCP::ToolResponse("{\"success\":true, \"temperature_c\":" + String(tempC) + "}");
  }
  );

  Serial.println("[MCP] 所有工具已註冊");
}

// === [核心] 持續背景讀取溫度 (非阻塞模式) ===
void handleTemperature() {
  // 狀態機變數
  static unsigned long lastRequestTime = 0;
  static bool waitingForConversion = false;
  unsigned long currentMillis = millis();

  // 設定讀取頻率 (每 3 秒更新一次數據)
  const unsigned long readInterval = 3000;
  // 溫度轉換時間 (12-bit 解析度安全值為 750ms，設 800ms 較保險)
  const unsigned long conversionDelay = 800;

  // 步驟 1: 發送讀取請求
  // 條件: 目前沒有在等待轉換，且距離上次請求已經超過設定的間隔
  if (!waitingForConversion && (currentMillis - lastRequestTime >= readInterval)) {
    // [重點] 因為 setup 設了 setWaitForConversion(false)，這行會瞬間執行完畢
    sensors.requestTemperatures();

    lastRequestTime = currentMillis; // 記錄請求時間
    waitingForConversion = true;     // 進入等待狀態
    // Serial.println("Temp request sent...");
  }

  // 步驟 2: 讀取數據
  // 條件: 正在等待轉換，且距離請求時間已經超過轉換所需的 800ms
  if (waitingForConversion && (currentMillis - lastRequestTime >= conversionDelay)) {
    float t = sensors.getTempCByIndex(0);

    // 過濾無效數值 (-127 為斷線)
    if (t > -100.0 && t != DEVICE_DISCONNECTED_C) {
      cachedTemp = t; // 更新全域變數
      // Serial.println("Temp updated: " + String(cachedTemp));
    }

    waitingForConversion = false; // 完成一次週期，回到閒置狀態
  }
}

// === 特效處理邏輯 ===
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

// === OLED 顯示內容繪製 ===
void drawOledContent() {
  u8g2.setFont(u8g2_font_6x12_tf);

  // 第一行：IP (Y=9)
  u8g2.setCursor(0, 9);
  if (WiFi.status() == WL_CONNECTED) {
    u8g2.print("IP:");
    u8g2.print(WiFi.localIP());
  } else {
    u8g2.print("Connecting...");
  }

  // 第二行：溫度與狀態 (Y=20)
  u8g2.setCursor(0, 20);
  u8g2.print("T:");
  if (cachedTemp > -100) {
    u8g2.print(cachedTemp, 1); // 顯示 1 位小數
  } else {
    u8g2.print("--"); // 初始化中
  }
  u8g2.print("C ");
  u8g2.print(connectionStatus);

  // 第三行：LED 模式 (Y=31)
  u8g2.setCursor(0, 31);
  u8g2.print("Mode: ");
  u8g2.print(currentModeName);
}

// === OLED 顯示處理 ===
void handleDisplay() {
  if (millis() - lastDisplayUpdate > 500) {
    lastDisplayUpdate = millis();

    u8g2.firstPage();
    do {
      drawOledContent();
    } while (u8g2.nextPage());
  }
}

// 輔助函數：彩虹色輪
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

  // === 初始化 I2C 腳位 (SDA 41, SCL 42) ===
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // 初始化 OLED
  u8g2.begin();
  u8g2.firstPage();
  do {
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(0, 10, "System Booting...");
  } while (u8g2.nextPage());

  // 初始化板載 LED
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  // 初始化 WS2812
  strip.begin();
  strip.show();
  strip.setBrightness(100);

  // === 初始化溫度傳感器 (OneWireNg + DallasTemperature) ===
  sensors.begin();

  // [關鍵設定] 關閉 "等待轉換"
  // 這行程式碼告訴函式庫不要使用 delay() 等待，讓我們實現非阻塞
  sensors.setWaitForConversion(false);

  Serial.println("DS18B20 Async Mode Initialized");

  // 連接 WiFi
  Serial.print("連接到 WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  // 連線中畫面
  u8g2.firstPage();
  do {
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(0, 10, "WiFi Connect...");
  } while (u8g2.nextPage());

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

  handleLedEffects();   // 處理 LED 特效
  handleDisplay();      // 處理 OLED 顯示
  handleTemperature();  // [新增] 週期性處理溫度讀取

  delay(5);             // 釋放 CPU 給 WiFi 堆疊
}
