#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include "PMS.h" // 使用您提供的自定義 Library

// === 硬體腳位定義 (ESP32-S3) ===
#define I2C_SDA_PIN 41
#define I2C_SCL_PIN 42
#define RX2_PIN 18     // PMS5003T TX -> ESP32-S3 RX2
#define TX2_PIN 17     // PMS5003T RX -> ESP32-S3 TX2

// === 物件初始化 ===
// 採用 Full Framebuffer 模式，確保 SSD1327 灰階顯示穩定不閃爍
U8G2_SSD1327_WS_128X128_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
PMS pms(Serial2); // 使用硬體 Serial2
PMS::DATA data;

void setup() {
  // 1. 初始化 USB Serial 用於電腦端除錯
  Serial.begin(115200);

  // 2. 初始化硬體 UART2 並指定腳位給 PMS5003T
  Serial2.begin(9600, SERIAL_8N1, RX2_PIN, TX2_PIN);

  // 3. 初始化 I2C (SDA: 41, SCL: 42)
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // 4. 初始化 OLED
  u8g2.begin();
  u8g2.setContrast(255); // 設定對比度為最大

  // 5. PMS5003T 模式設定
  pms.passiveMode(); // 切換至被動模式
  pms.wakeUp();      // 喚醒感測器

  Serial.println("System Initialized. Reading PMS5003T...");
}

void loop() {
  // 請求數據讀取
  pms.requestRead();

  // 嘗試在 1 秒內讀取數據
  if (pms.readUntil(data)) {
    u8g2.clearBuffer();
    u8g2.setDrawColor(15); // SSD1327 灰階最大值（純白）
    u8g2.setFontMode(1);   // 透明背景模式

    // --- 介面設計：頂部標題區 ---
    u8g2.drawFrame(0, 0, 128, 128); // 外框
    u8g2.setFont(u8g2_font_6x13_tr);
    u8g2.drawStr(25, 20, "AIR MONITOR");
    u8g2.drawLine(10, 25, 118, 25); // 分隔線

    // --- 數據顯示區：PM2.5 ---
    u8g2.setFont(u8g2_font_7x14_tr);
    u8g2.drawStr(15, 45, "PM2.5:");
    u8g2.setCursor(65, 45);
    u8g2.print(data.PM_AE_UG_2_5);
    u8g2.drawStr(100, 45, "ug");

    // --- 數據顯示區：溫濕度 (採用 Helvetica 粗體) ---
    u8g2.setFont(u8g2_font_helvB12_tr);

    // 溫度顯示 (配合您 Lib 中的變數名 Tmperature 並除以 10.0)
    u8g2.drawStr(10, 75, "Tem:");
    u8g2.setCursor(55, 75);
    u8g2.print(data.Tmperature / 10.0, 1);
    u8g2.drawStr(105, 75, "C");

    // 濕度顯示
    u8g2.drawStr(10, 95, "Hum:");
    u8g2.setCursor(55, 95);
    u8g2.print(data.Humidity / 10.0, 1);
    u8g2.drawStr(105, 95, "%");

    // --- 底部裝飾與幾何圖形 ---
    u8g2.drawLine(10, 103, 118, 103);
    u8g2.drawDisc(35, 114, 6);        // 實心圓
    u8g2.drawCircle(64, 114, 6);      // 空心圓
    u8g2.drawBox(90, 108, 12, 12);    // 實心矩形

    u8g2.sendBuffer(); // 送出至螢幕

    // 同步輸出至序列埠
    Serial.printf("PM2.5: %d, Temp: %.1f, Humi: %.1f\n",
                  data.PM_AE_UG_2_5, data.Tmperature / 10.0, data.Humidity / 10.0);
  } else {
    Serial.println("Waiting for sensor data...");
  }

  delay(2000); // 建議每 2 秒更新一次
}
