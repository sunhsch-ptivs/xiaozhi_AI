#include <Arduino.h>
#include <U8g2lib.h>

#ifdef U8X8_HAVE_HW_SPI
#include <SPI.h>
#endif
#ifdef U8X8_HAVE_HW_I2C
#include <Wire.h>
#endif

// === 您保留的建構函式 (U8X8 模式) ===
// 注意：這是 "文字模式" (U8x8)，不支援繪圖(畫線/圓)和部分字型
// 適用於 Waveshare 1.5inch OLED 或相容模組
U8X8_SSD1327_WS_128X128_HW_I2C u8g2(U8X8_PIN_NONE);

// === 硬體腳位定義 ===
// 您的腳位 41/42 暗示您可能使用的是 ESP32-S3 等高階板子
#define I2C_SDA_PIN 41 // OLED SDA
#define I2C_SCL_PIN 42 // OLED SCL

void setup(void) {
  // 1. 初始化 I2C 通訊 (針對 ESP32 自定義腳位)
  // 如果是標準 Arduino Uno/Nano，通常不需要指定腳位，直接 Wire.begin() 即可
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // 2. 初始化 OLED
  u8g2.begin();
  
  // 3. 設定省電模式為 0 (開啟螢幕)
  u8g2.setPowerSave(0);
  
  // 4. 設定字型 (U8x8 只能使用 u8x8 系列字型)
  u8g2.setFont(u8x8_font_chroma48medium8_r);
  
  // 5. 顯示歡迎訊息
  u8g2.drawString(0, 0, "System Ready");
  u8g2.drawString(0, 2, "128x128 OLED");
  u8g2.drawString(0, 4, "Mode: U8X8");
  
  delay(2000); // 暫停兩秒看開機畫面
  u8g2.clearDisplay(); // 清除螢幕
}

void loop(void) {
  // 顯示標題
  u8g2.setFont(u8x8_font_chroma48medium8_r);
  u8g2.inverse(); // 反白顯示
  u8g2.drawString(0, 0, "  SENSOR DATA  ");
  u8g2.noInverse(); // 取消反白

  // 模擬顯示數據 (U8x8 採用 Grid 定位，1 Grid = 8x8 pixels)
  // 所以 (0, 2) 代表第 0 列，第 16 pixel 高度的位置
  u8g2.drawString(0, 2, "Temp: 28.5 C");
  
  u8g2.drawString(0, 4, "Humi: 65.0 %");
  
  // 顯示運行時間 (Millis)
  u8g2.drawString(0, 6, "Time:");
  u8g2.setCursor(6, 6); // 設定游標位置 (Grid 座標)
  u8g2.print(millis() / 1000);
  u8g2.print(" s");

  // 製作簡單的閃爍效果
  static bool blink = false;
  u8g2.drawString(0, 8, blink ? "Status: RUNNING" : "Status:        ");
  blink = !blink;

  delay(500);
}
