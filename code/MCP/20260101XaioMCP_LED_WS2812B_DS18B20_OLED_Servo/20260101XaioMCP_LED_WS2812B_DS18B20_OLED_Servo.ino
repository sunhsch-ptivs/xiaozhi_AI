/*
 * 專案: ESP32-S3 小智 MCP 控制器 (保溫杯全功能版 - 溫控 + 伺服 + 倒數計時器 + 55度過熱保護)
 * 硬體配置:
 * - 加熱器 (MOSFET): GPIO 47
 * - 伺服馬達 (Servo): GPIO 21
 * - WS2812B: GPIO 40 (5顆)
 * - DS18B20: GPIO 48 (OneWireNg)
 * - OLED: SDA 41, SCL 42 (128x32)
 */

#include <WiFi.h>
#include <ArduinoJson.h>
#include <WebSocketMCP.h>
#include <Adafruit_NeoPixel.h>

// === 1. 伺服馬達庫 ===
#include <ESP32Servo.h>

// === 2. 溫度相關庫 ===
#include <OneWire.h> 
#include <DallasTemperature.h>

// === 3. OLED 顯示庫 ===
#include <Arduino.h>
#include <U8g2lib.h>

#ifdef U8X8_HAVE_HW_SPI
#include <SPI.h>
#endif
#ifdef U8X8_HAVE_HW_I2C
#include <Wire.h>
#endif

// [OLED 設定] 128x32
U8G2_SSD1306_128X32_UNIVISION_1_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// === 硬體腳位定義 ===
#define HEATER_PIN 47       // 加熱器控制腳位
#define WS2812_PIN 40       
#define NUM_LEDS 5          // LED 數量
#define ONE_WIRE_BUS 48     // DS18B20 腳位
#define I2C_SDA_PIN 41      // OLED SDA
#define I2C_SCL_PIN 42      // OLED SCL
#define SERVO_PIN 21        // 伺服馬達訊號腳位

// === [溫度與安全設定] ===
#define TEMP_ALERT_LIMIT 37.0 // 警示溫度
#define MAX_TEMP_LIMIT   55.0 // [修改] 過熱保護溫度提升至 55.0度

// === WiFi 配置 ===
const char* ssid = "louisguan";
const char* password = "0989839679";

// === MCP 服務器配置 ===
const char* mcpEndpoint = "wss://api.xiaozhi.me/mcp/?token=eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCJ9.eyJ1c2VySWQiOjQwNzc5MywiYWdlbnRJZCI6MTEzMTg2MywiZW5kcG9pbnRJZCI6ImFnZW50XzExMzE4NjMiLCJwdXJwb3NlIjoibWNwLWVuZHBvaW50IiwiaWF0IjoxNzY3Mjc0NDUzLCJleHAiOjE3OTg4MzIwNTN9.xbHT8F2-_qNJFFScpSv3ALAo1Au0-pKXt1opzsTvf8KDmNaShC_4clDgXfeD7lbm3I48eCZqPMI7YUiBmQojmw";

// === 全局物件 ===
WebSocketMCP mcpClient;
Adafruit_NeoPixel strip(NUM_LEDS, WS2812_PIN, NEO_GRB + NEO_KHZ800);
Servo myServo;

// [溫度物件初始化]
OneWire oneWire(ONE_WIRE_BUS); 
DallasTemperature sensors(&oneWire);

// === 狀態變數 ===
String connectionStatus = "Connecting...";
String currentModeName = "None";
String heaterStatus = "OFF"; 
float cachedTemp = -127.0; 
unsigned long lastDisplayUpdate = 0;

// [記憶加熱器是否開啟的變數]
bool isHeaterOn = false; 

// === 伺服馬達狀態管理 ===
bool isServoActive = false;
unsigned long servoDetachTime = 0;
int currentServoAngle = 85;

// === 計時器變數 ===
enum TimerState { TIMER_STOPPED, TIMER_RUNNING, TIMER_PAUSED, TIMER_ALARM };
TimerState timerState = TIMER_STOPPED;
long timerRemainingMs = 0;      // 剩餘毫秒數
unsigned long timerLastUpdate = 0; // 上次計算時間戳
String timerDisplayStr = "";    // 供 OLED 顯示的時間字串

// === 燈光特效狀態 ===
enum LedEffect {
  EFFECT_NONE,            // 待機模式 (自動監控溫度)
  EFFECT_RAINBOW,         // 加熱中
  EFFECT_THEATER_RAINBOW, // 手動指令
  EFFECT_THEATER_RED,     // 手動指令
  EFFECT_THEATER_ICE_BLUE,// 計時器響鈴 (冰藍色)
  EFFECT_COLOR_WIPE,      // 手動指令
  EFFECT_BLINK_AND_STOP   // 伺服馬達動作中
};

LedEffect currentEffect = EFFECT_NONE;
unsigned long lastEffectUpdate = 0;
int effectStep = 0;
uint32_t wipeColor = 0;
uint32_t blinkTargetColor = 0;

// --- 函數前向宣告 ---
void registerMcpTools();
void onConnectionStatus(bool connected);
void handleLedEffects();
void handleDisplay(); 
void handleTemperature(); 
void handleServo();
void handleTimer(); 
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
  
  // 1. 保溫杯加熱器控制工具
  mcpClient.registerTool(
    "heater_control",
    "保溫杯加熱器 (MOSFET) 控制與狀態查詢。On=開啟加熱(彩虹燈), Off=關閉加熱(若>37度自動警示), Status=查詢目前開關狀態",
    "{\"type\":\"object\",\"properties\":{\"state\":{\"type\":\"string\",\"enum\":[\"on\",\"off\",\"status\"]}},\"required\":[\"state\"]}",
    [](const String & args) {
      DynamicJsonDocument doc(256);
      deserializeJson(doc, args);
      String state = doc["state"].as<String>();
      String msg = "";

      if (state == "on") {
        if (cachedTemp >= MAX_TEMP_LIMIT) {
           // 這裡也要修改錯誤訊息，避免 AI 混淆
           return WebSocketMCP::ToolResponse("{\"success\":false,\"error\":\"Temp > 55C, Heater Blocked!\",\"is_heating\":false}");
        }
        digitalWrite(HEATER_PIN, HIGH);
        heaterStatus = "ON";
        isHeaterOn = true; 
        currentEffect = EFFECT_RAINBOW;
        effectStep = 0;
        currentModeName = "HeaterON(Rainbow)";
        msg = "Heater turned ON";
      } 
      else if (state == "off") {
        digitalWrite(HEATER_PIN, LOW);
        heaterStatus = "OFF";
        isHeaterOn = false;
        currentEffect = EFFECT_NONE; 
        currentModeName = "Monitor Mode";
        msg = "Heater turned OFF";
      }
      else if (state == "status") {
        msg = "Status query success";
      }

      String jsonResponse = "{";
      jsonResponse += "\"success\":true,";
      jsonResponse += "\"msg\":\"" + msg + "\",";
      jsonResponse += "\"heater_state\":\"" + String(isHeaterOn ? "on" : "off") + "\","; 
      jsonResponse += "\"is_heating\":" + String(isHeaterOn ? "true" : "false") + ",";   
      jsonResponse += "\"temp_c\":" + String(cachedTemp);                                 
      jsonResponse += "}";
      return WebSocketMCP::ToolResponse(jsonResponse);
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
    "控制WS2812燈條特效。Mode: clear(回到自動溫控), rainbow, theater, wipe, single",
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
        effectStep = 0; 
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

  // 3. 讀取溫度工具
  mcpClient.registerTool(
    "read_temperature",
    "讀取 DS18B20 保溫杯的溫度 (攝氏)",
    "{}",
    [](const String& args) {
      float tempC = cachedTemp;
      return WebSocketMCP::ToolResponse("{\"success\":true, \"temperature_c\":" + String(tempC) + ", \"is_heating\":" + String(isHeaterOn ? "true":"false") + "}");
    }
  );

  // 4. 伺服馬達控制工具
  const char* servoSchema = 
    "{"
      "\"type\": \"object\","
      "\"properties\": {"
        "\"action\": { \"type\": \"string\", \"enum\": [\"open\", \"close\", \"set\"], \"description\": \"Open(150), Close(85), or Set specific angle\" },"
        "\"angle\": { \"type\": \"integer\", \"description\": \"Target angle (85-150), required if action is 'set'\" }"
      "},"
      "\"required\": [\"action\"]"
    "}";

  mcpClient.registerTool(
    "control_box",
    "控制藥盒/支撐腳架開關 (伺服馬達)。Open=打開(150度)並亮起彩虹燈, Close=關閉(85度)並關燈, Set=指定角度",
    servoSchema,
    [](const String& args) {
      DynamicJsonDocument doc(256);
      deserializeJson(doc, args);
      
      String action = doc["action"].as<String>();
      int targetAngle = 85; 

      if (action == "open") {
        targetAngle = 150;
        currentEffect = EFFECT_BLINK_AND_STOP;
        blinkTargetColor = strip.Color(0, 255, 0); 
        effectStep = 0; 
        currentModeName = "Open(BlinkGreen)"; 
      } 
      else if (action == "close") {
        targetAngle = 85;
        currentEffect = EFFECT_BLINK_AND_STOP;
        blinkTargetColor = strip.Color(255, 20, 147); 
        effectStep = 0; 
        currentModeName = "Close(BlinkPink)";
      } 
      else if (action == "set") {
        targetAngle = doc["angle"] | 85;
      }

      if (targetAngle < 85) targetAngle = 85;
      if (targetAngle > 150) targetAngle = 150;

      myServo.attach(SERVO_PIN);
      myServo.write(targetAngle);
      
      currentServoAngle = targetAngle;
      isServoActive = true;
      servoDetachTime = millis() + 800;
      Serial.println("Servo moving to: " + String(targetAngle));

      return WebSocketMCP::ToolResponse("{\"success\":true, \"angle\":" + String(targetAngle) + ", \"status\":\"Moving\"}");
    }
  );

  // 5. 倒數計時器工具
  const char* timerSchema = 
    "{"
      "\"type\": \"object\","
      "\"properties\": {"
        "\"command\": { \"type\": \"string\", \"enum\": [\"start\", \"pause\", \"resume\", \"cancel\", \"status\"] },"
        "\"minutes\": { \"type\": \"integer\", \"description\": \"Duration in minutes (multiple of 15, max 360). Required for 'start'.\" }"
      "},"
      "\"required\": [\"command\"]"
    "}";

  mcpClient.registerTool(
    "timer_control",
    "倒數計時器功能(喝水/吃藥提醒)。Start(需指定分鐘, 15的倍數, Max 6小時), Pause, Resume, Cancel, Status(查詢剩餘時間)",
    timerSchema,
    [](const String& args) {
      DynamicJsonDocument doc(256);
      deserializeJson(doc, args);
      String cmd = doc["command"].as<String>();
      String msg = "";
      bool success = true;

      if (cmd == "start") {
         int mins = doc["minutes"] | 0;
         if (mins <= 0 || mins > 360 || (mins % 15 != 0)) {
            success = false;
            msg = "Invalid duration. Must be multiple of 15 min, max 360 min.";
         } else {
            timerRemainingMs = (long)mins * 60 * 1000;
            timerLastUpdate = millis();
            timerState = TIMER_RUNNING;
            msg = "Timer started for " + String(mins) + " mins.";
            
            if (currentEffect == EFFECT_THEATER_ICE_BLUE) {
                currentEffect = EFFECT_NONE;
            }
         }
      } 
      else if (cmd == "pause") {
         if (timerState == TIMER_RUNNING) {
             timerState = TIMER_PAUSED;
             msg = "Timer paused.";
         } else {
             msg = "Timer is not running.";
         }
      }
      else if (cmd == "resume") {
         if (timerState == TIMER_PAUSED) {
             timerState = TIMER_RUNNING;
             timerLastUpdate = millis(); 
             msg = "Timer resumed.";
         } else {
             msg = "Timer is not paused.";
         }
      }
      else if (cmd == "cancel") {
         timerState = TIMER_STOPPED;
         timerRemainingMs = 0;
         msg = "Timer cancelled.";
         if (currentEffect == EFFECT_THEATER_ICE_BLUE) {
             currentEffect = EFFECT_NONE; 
         }
      }
      else if (cmd == "status") {
         msg = "Status query.";
      }

      String timeStr = "00:00:00";
      if (timerState != TIMER_STOPPED) {
          long totalSec = timerRemainingMs / 1000;
          if (totalSec < 0) totalSec = 0;
          int h = totalSec / 3600;
          int m = (totalSec % 3600) / 60;
          int s = totalSec % 60;
          char buf[10];
          sprintf(buf, "%02d:%02d:%02d", h, m, s);
          timeStr = String(buf);
      }

      String stateStr = "stopped";
      if (timerState == TIMER_RUNNING) stateStr = "running";
      else if (timerState == TIMER_PAUSED) stateStr = "paused";
      else if (timerState == TIMER_ALARM) stateStr = "alarm";

      String jsonResp = "{";
      jsonResp += "\"success\":" + String(success ? "true":"false") + ",";
      jsonResp += "\"msg\":\"" + msg + "\",";
      jsonResp += "\"state\":\"" + stateStr + "\",";
      jsonResp += "\"remaining_str\":\"" + timeStr + "\",";
      jsonResp += "\"remaining_ms\":" + String(timerRemainingMs);
      jsonResp += "}";

      return WebSocketMCP::ToolResponse(jsonResp);
    }
  );

  Serial.println("[MCP] 所有工具已註冊");
}

// === 伺服馬達自動管理邏輯 ===
void handleServo() {
  if (isServoActive && millis() > servoDetachTime) {
    myServo.detach();
    isServoActive = false;
    Serial.println("Servo Detached");
  }
}

// === 計時器邏輯 ===
void handleTimer() {
    if (timerState == TIMER_RUNNING) {
        unsigned long now = millis();
        long elapsed = now - timerLastUpdate;
        timerLastUpdate = now;

        timerRemainingMs -= elapsed;

        if (timerRemainingMs <= 0) {
            timerRemainingMs = 0;
            timerState = TIMER_ALARM;
            Serial.println("Timer Finished! Alarm triggered.");
            
            // 觸發冰藍色燈效
            currentEffect = EFFECT_THEATER_ICE_BLUE;
            effectStep = 0;
            currentModeName = "Time's UP!";
        }
    }
    
    // 更新顯示字串
    static unsigned long lastStrUpdate = 0;
    if (millis() - lastStrUpdate > 1000) {
        lastStrUpdate = millis();
        if (timerState == TIMER_RUNNING || timerState == TIMER_PAUSED) {
            long totalSec = timerRemainingMs / 1000;
            int m = totalSec / 60; 
            int s = totalSec % 60;
            char buf[16];
            if (totalSec >= 3600) {
                 sprintf(buf, "%dh %02dm", totalSec/3600, (totalSec%3600)/60);
            } else {
                 sprintf(buf, "%02d:%02d", m, s);
            }
            timerDisplayStr = String(buf);
        } else if (timerState == TIMER_ALARM) {
            timerDisplayStr = "ALARM!";
        } else {
            timerDisplayStr = "";
        }
    }
}

// === 溫度背景讀取邏輯 (包含 55度 過熱保護) ===
void handleTemperature() {
  static unsigned long lastRequestTime = 0;
  static bool waitingForConversion = false;
  unsigned long currentMillis = millis();
  
  const unsigned long readInterval = 3000;
  const unsigned long conversionDelay = 800; 

  if (!waitingForConversion && (currentMillis - lastRequestTime >= readInterval)) {
    sensors.requestTemperatures();
    lastRequestTime = currentMillis;
    waitingForConversion = true;
  }

  if (waitingForConversion && (currentMillis - lastRequestTime >= conversionDelay)) {
    float t = sensors.getTempCByIndex(0);
    if (t > -100.0 && t != DEVICE_DISCONNECTED_C) {
      cachedTemp = t;

      if (isHeaterOn && cachedTemp >= MAX_TEMP_LIMIT) {
        Serial.println("!!! ALERT: Temp > 55C. Force Stop Heater !!!");
        digitalWrite(HEATER_PIN, LOW);
        heaterStatus = "OFF (Auto-Protect)";
        isHeaterOn = false;
        
        currentEffect = EFFECT_NONE;
        currentModeName = "Auto-Protect(>55C)";
      }
    }
    waitingForConversion = false; 
  }
}

// === 特效處理邏輯 (包含狀態回朔與溫度監控) ===
void handleLedEffects() {
  unsigned long currentMillis = millis();
  
  switch (currentEffect) {
    // 待機 / 溫度監控模式
    case EFFECT_NONE:
    default:
      if (cachedTemp >= TEMP_ALERT_LIMIT) {
         if (currentMillis - lastEffectUpdate > 80) { 
            lastEffectUpdate = currentMillis;
            for (int i = 0; i < strip.numPixels(); i = i + 3) {
              strip.setPixelColor(i + ((effectStep + 2) % 3), 0);
            }
            int q = effectStep % 3;
            for (int i = 0; i < strip.numPixels(); i = i + 3) {
              if (i + q < strip.numPixels()) {
                strip.setPixelColor(i + q, 255, 0, 0); 
              }
            }
            strip.show();
            effectStep++;
            if (effectStep >= 3) effectStep = 0; 
         }
      } else {
         strip.clear();
         strip.show();
         effectStep = 0;
      }
      break;

    // 計時器響鈴：冰藍色 Theater
    case EFFECT_THEATER_ICE_BLUE:
      if (currentMillis - lastEffectUpdate > 80) {
        lastEffectUpdate = currentMillis;
        uint32_t iceColor = strip.Color(0, 191, 255); 

        for (int i = 0; i < strip.numPixels(); i = i + 3) {
          strip.setPixelColor(i + ((effectStep + 2) % 3), 0);
        }
        int q = effectStep % 3;
        for (int i = 0; i < strip.numPixels(); i = i + 3) {
          if (i + q < strip.numPixels()) {
            strip.setPixelColor(i + q, iceColor);
          }
        }
        strip.show();
        effectStep++;
        if (effectStep >= 3) effectStep = 0; 
      }
      break;

    case EFFECT_BLINK_AND_STOP:
      if (currentMillis - lastEffectUpdate > 250) { 
        lastEffectUpdate = currentMillis;
        if (effectStep < 6) {
          if (effectStep % 2 == 0) strip.fill(blinkTargetColor);
          else strip.clear();
          strip.show();
          effectStep++;
        } else {
          // 閃爍結束，恢復原本狀態
          if (isHeaterOn) {
            if (cachedTemp >= MAX_TEMP_LIMIT) {
                isHeaterOn = false;
                digitalWrite(HEATER_PIN, LOW);
                currentEffect = EFFECT_NONE;
            } else {
                currentEffect = EFFECT_RAINBOW;
                effectStep = 0;
                currentModeName = "HeaterON(Rainbow)";
            }
          } else {
            currentEffect = EFFECT_NONE;
            currentModeName = "Monitor Mode";
          }
        }
      }
      break;

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
      
    case EFFECT_THEATER_RED:
       if (currentMillis - lastEffectUpdate > 80) {
        lastEffectUpdate = currentMillis;
        for (int i = 0; i < strip.numPixels(); i = i + 3) {
          strip.setPixelColor(i + ((effectStep + 2) % 3), 0);
        }
        int q = effectStep % 3;
        for (int i = 0; i < strip.numPixels(); i = i + 3) {
          if (i + q < strip.numPixels()) {
            strip.setPixelColor(i + q, 255, 0, 0);
          }
        }
        strip.show();
        effectStep++;
        if (effectStep >= 3) effectStep = 0; 
      }
      break;
  }
}

// === OLED 顯示內容繪製 ===
void drawOledContent() {
  u8g2.setFont(u8g2_font_6x12_tf); 

  // IP
  u8g2.setCursor(0, 9);
  if (WiFi.status() == WL_CONNECTED) {
    u8g2.print("IP:");
    u8g2.print(WiFi.localIP());
  } else {
    u8g2.print("Connecting...");
  }

  // 溫度與加熱狀態
  u8g2.setCursor(0, 20);
  u8g2.print("T:");
  if (cachedTemp > -100) {
    u8g2.print(cachedTemp, 1);
    if (cachedTemp >= TEMP_ALERT_LIMIT) u8g2.print("!"); 
  } else {
    u8g2.print("--");
  }
  u8g2.print("C H:"); 
  u8g2.print(heaterStatus);

  // 第三行：LED 模式 或 計時器狀態
  u8g2.setCursor(0, 31); 
  
  if (timerState != TIMER_STOPPED) {
      if (timerState == TIMER_PAUSED) u8g2.print("Pau:");
      else if (timerState == TIMER_ALARM) u8g2.print("ALARM:");
      else u8g2.print("Tmr:");
      
      u8g2.print(timerDisplayStr);
  } 
  else {
      u8g2.print("M:");
      if (currentModeName == "None" || currentModeName == "Monitor Mode" || currentModeName == "Auto-Protect(>55C)") {
         if (cachedTemp >= TEMP_ALERT_LIMIT) u8g2.print("Alert! >37");
         else u8g2.print("Monitor");
      } else {
         u8g2.print(currentModeName);
      }
  }
}

void handleDisplay() {
  if (millis() - lastDisplayUpdate > 500) {
    lastDisplayUpdate = millis();
    u8g2.firstPage();
    do { drawOledContent(); } while (u8g2.nextPage());
  }
}

uint32_t Wheel(byte WheelPos) {
  WheelPos = 255 - WheelPos;
  if (WheelPos < 85) return strip.Color(255 - WheelPos * 3, 0, WheelPos * 3);
  if (WheelPos < 170) { WheelPos -= 85; return strip.Color(0, WheelPos * 3, 255 - WheelPos * 3); }
  WheelPos -= 170; return strip.Color(WheelPos * 3, 255 - WheelPos * 3, 0);
}

void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  u8g2.begin();
  u8g2.firstPage();
  do { u8g2.setFont(u8g2_font_6x12_tf); u8g2.drawStr(0, 10, "System Booting..."); } while (u8g2.nextPage());

  pinMode(HEATER_PIN, OUTPUT);
  digitalWrite(HEATER_PIN, LOW); 

  strip.begin();
  strip.show();
  strip.setBrightness(100);
  
  sensors.begin();
  sensors.setWaitForConversion(false);
  
  myServo.setPeriodHertz(50); 
  myServo.attach(SERVO_PIN, 500, 2400); 
  myServo.write(85);
  delay(500); 
  myServo.detach();
  currentServoAngle = 85;

  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  u8g2.firstPage(); 
  do { u8g2.drawStr(0, 10, "WiFi Connect..."); } while (u8g2.nextPage());
  
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }

  Serial.println("\nWiFi Connected");
  mcpClient.begin(mcpEndpoint, onConnectionStatus);
}

void loop() {
  mcpClient.loop();
  handleLedEffects();   
  handleDisplay();      
  handleTemperature();  
  handleServo(); 
  handleTimer(); 
  delay(5);
}
