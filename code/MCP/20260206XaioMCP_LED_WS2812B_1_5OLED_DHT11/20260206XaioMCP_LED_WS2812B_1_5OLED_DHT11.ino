#include <WiFi.h>
#include <ArduinoJson.h>
#include <WebSocketMCP.h>
#include <Adafruit_NeoPixel.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <DHT.h>
#include <DHT_U.h>

// =======================
// 1. 硬體腳位定義 (ESP32-S3 N8R8)
// =======================
// 燈光與指示燈
#define LED_PIN 8           // 板載 LED
#define WS2812_PIN 40       // WS2812 數據腳位
#define NUM_LEDS 5          // LED 數量

// OLED 與 感測器
#define I2C_SDA_PIN 41
#define I2C_SCL_PIN 42
#define DHTPIN 11           // DHT11 數據腳位
#define DHTTYPE DHT11       // 感測器型號

// =======================
// 2. 網路與 MCP 設定
// =======================
const char* ssid = "louisguan";
const char* password = "0989839679";
const char* mcpEndpoint = "wss://api.xiaozhi.me/mcp/?token=eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9.eyJ1c2VySWQiOjQwNzc5MywiYWdlbnRJZCI6MTQzNDQzOSwiZW5kcG9pbnRJZCI6ImFnZW50XzE0MzQ0MzkiLCJwdXJwb3NlIjoibWNwLWVuZHBvaW50IiwiaWF0IjoxNzcwMjc3NzQyLCJleHAiOjE4MDE4MzUzNDJ9.6Whb39kVikeF0qf9xP-F-QybnTDNXr-3uixCmrKAdy2ZWGaEDOvQMlqjGvsIpVqB1NqeFhYn1lcWFEjkmjR0ug";

// =======================
// 3. 全局物件宣告
// =======================
WebSocketMCP mcpClient;

// 燈條物件
Adafruit_NeoPixel strip(NUM_LEDS, WS2812_PIN, NEO_GRB + NEO_KHZ800);

// OLED 物件 (使用硬體 I2C)
U8G2_SSD1327_WS_128X128_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// DHT 物件
DHT dht(DHTPIN, DHTTYPE);

// =======================
// 4. 變數管理
// =======================
// 燈光特效狀態
enum LedEffect { EFFECT_NONE, EFFECT_RAINBOW, EFFECT_THEATER_RAINBOW, EFFECT_COLOR_WIPE };
LedEffect currentEffect = EFFECT_NONE;
unsigned long lastEffectUpdate = 0;
int effectStep = 0;
uint32_t wipeColor = 0;

// 感測器數據緩存 (供 MCP 讀取使用)
float currentTemp = 0.0;
float currentHum = 0.0;
unsigned long lastSensorUpdate = 0; // 用於非阻塞計時

// =======================
// 5. 函數宣告
// =======================
void registerMcpTools();
void onConnectionStatus(bool connected);
void handleLedEffects();
void handleSensorsAndDisplay();
uint32_t Wheel(byte WheelPos);

// =======================
// 6. MCP 回調與工具註冊
// =======================
void onConnectionStatus(bool connected) {
  if (connected) {
    Serial.println("[MCP] 已連接到服務器");
    registerMcpTools();
  } else {
    Serial.println("[MCP] 與服務器斷開連接");
  }
}

void registerMcpTools() {
  // --- 工具 1: 板載 LED ---
  mcpClient.registerTool("led_blink", "控制ESP32板載LED (開/關/閃爍)", 
    "{\"type\":\"object\",\"properties\":{\"state\":{\"type\":\"string\",\"enum\":[\"on\",\"off\",\"blink\"]}},\"required\":[\"state\"]}",
  [](const String & args) {
    DynamicJsonDocument doc(256);
    deserializeJson(doc, args);
    String state = doc["state"].as<String>();
    if (state == "on") digitalWrite(LED_PIN, HIGH);
    else if (state == "off") digitalWrite(LED_PIN, LOW);
    else if (state == "blink") {
      for (int i = 0; i < 3; i++) {
        digitalWrite(LED_PIN, HIGH); delay(200); // 這裡的短暫 delay 是可以接受的
        digitalWrite(LED_PIN, LOW); delay(200);
      }
    }
    return WebSocketMCP::ToolResponse("{\"success\":true,\"state\":\"" + state + "\"}");
  });

  // --- 工具 2: WS2812B 燈光控制 ---
  const char* ws2812Schema = "{\"type\": \"object\",\"properties\": {\"mode\": { \"type\": \"string\", \"enum\": [\"rainbow\", \"theater\", \"wipe\", \"single\", \"clear\"] },\"index\": { \"type\": \"integer\" },\"r\": { \"type\": \"integer\" },\"g\": { \"type\": \"integer\" },\"b\": { \"type\": \"integer\" }},\"required\": [\"mode\"]}";
  
  mcpClient.registerTool("ws2812_control", "控制WS2812特效 (rainbow, theater, wipe, clear) 或單點 (single)", ws2812Schema,
  [](const String & args) {
    DynamicJsonDocument doc(512);
    deserializeJson(doc, args);
    String mode = doc["mode"].as<String>();
    int r = doc["r"] | 0; int g = doc["g"] | 0; int b = doc["b"] | 0;

    if (mode == "rainbow") { currentEffect = EFFECT_RAINBOW; effectStep = 0; }
    else if (mode == "theater") { currentEffect = EFFECT_THEATER_RAINBOW; effectStep = 0; }
    else if (mode == "wipe") { currentEffect = EFFECT_COLOR_WIPE; wipeColor = strip.Color(r, g, b); effectStep = 0; }
    else if (mode == "clear") { currentEffect = EFFECT_NONE; strip.clear(); strip.show(); }
    else if (mode == "single") {
      currentEffect = EFFECT_NONE;
      int idx = doc["index"];
      if (idx >= 0 && idx < NUM_LEDS) {
        strip.setPixelColor(idx, strip.Color(r, g, b));
        strip.show();
      } else {
        return WebSocketMCP::ToolResponse("{\"success\":false,\"error\":\"Index out of range\"}");
      }
    }
    return WebSocketMCP::ToolResponse("{\"success\":true,\"mode\":\"" + mode + "\"}");
  });

  // --- 工具 3: 讀取環境數據 (新功能!) ---
  mcpClient.registerTool("get_env_data", "讀取目前的溫度與濕度", "{}", 
  [](const String & args) {
    // 直接回傳最新讀取的全域變數
    String json = "{\"temperature\":" + String(currentTemp) + ",\"humidity\":" + String(currentHum) + "}";
    return WebSocketMCP::ToolResponse(json);
  });

  Serial.println("[MCP] 所有工具已註冊 (LED, WS2812, Sensor)");
}

// =======================
// 7. SETUP 初始化
// =======================
void setup() {
  Serial.begin(115200);
  
  // 1. 初始化 I2C 與 OLED (放在前面確保 I2C 總線啟動)
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  u8g2.begin();
  u8g2.setContrast(255); // 最大亮度
  
  // 顯示啟動畫面
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x13_tr);
  u8g2.drawStr(10, 40, "System Booting...");
  u8g2.drawStr(10, 60, "Connecting WiFi...");
  u8g2.sendBuffer();

  // 2. 初始化感測器
  dht.begin();
  
  // 3. 初始化 LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  strip.begin();
  strip.show();
  strip.setBrightness(100);

  // 4. 連接 WiFi
  Serial.print("連接 WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi Connected: " + WiFi.localIP().toString());

  // 更新 OLED 顯示 IP
  u8g2.clearBuffer();
  u8g2.drawStr(10, 40, "WiFi OK!");
  u8g2.drawStr(0, 60, WiFi.localIP().toString().c_str());
  u8g2.sendBuffer();
  delay(1000); // 讓使用者看到 IP

  // 5. 初始化 MCP
  mcpClient.begin(mcpEndpoint, onConnectionStatus);
}

// =======================
// 8. LOOP 主循環
// =======================
void loop() {
  mcpClient.loop();          // 處理 MCP 通訊 (最優先)
  handleLedEffects();        // 處理燈光特效 (非阻塞)
  handleSensorsAndDisplay(); // 處理感測器與螢幕 (非阻塞)
  
  delay(5); // 極短暫延遲讓 CPU 喘息
}

// =======================
// 9. 功能實作
// =======================

// --- 處理感測器讀取與螢幕繪製 (每 2 秒一次) ---
void handleSensorsAndDisplay() {
  unsigned long currentMillis = millis();
  
  // DHT11 建議讀取間隔至少 2 秒，不要太頻繁讀取
  if (currentMillis - lastSensorUpdate > 2000) {
    lastSensorUpdate = currentMillis;

    // 讀取數據
    float h = dht.readHumidity();
    float t = dht.readTemperature();

    // 檢查錯誤，若成功則更新全域變數
    if (!isnan(h) && !isnan(t)) {
      currentTemp = t;
      currentHum = h;
      Serial.printf("Temp: %.1f C, Hum: %.1f %%\n", t, h);
    } else {
      Serial.println("DHT Sensor Error!");
    }

    // 更新 OLED (繪圖邏輯移植自原程式)
    u8g2.clearBuffer();
    u8g2.setDrawColor(15);
    u8g2.setFontMode(1);

    // 外框
    u8g2.drawFrame(0, 0, 128, 128);

    // 標題
    u8g2.setFont(u8g2_font_6x13_tr);
    u8g2.drawStr(25, 20, "ENV MONITOR");
    u8g2.drawLine(10, 25, 118, 25);

    if (isnan(h) || isnan(t)) {
      u8g2.drawStr(20, 65, "Sensor Error!");
    } else {
      // 溫度
      u8g2.setFont(u8g2_font_helvB12_tr);
      u8g2.drawStr(10, 55, "Tem:");
      u8g2.drawStr(55, 55, String(t).c_str());
      u8g2.drawStr(105, 55, "C");

      // 濕度
      u8g2.drawStr(10, 85, "Hum:");
      u8g2.drawStr(55, 85, String(h).c_str());
      u8g2.drawStr(105, 85, "%");
    }

    // 底部裝飾
    u8g2.drawLine(10, 100, 118, 100);
    u8g2.drawDisc(35, 114, 8);
    u8g2.drawCircle(64, 114, 8);
    u8g2.drawBox(90, 106, 16, 16);

    u8g2.sendBuffer();
  }
}

// --- 處理 LED 特效 (移植自原程式) ---
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

// 彩虹色輪輔助函數
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
