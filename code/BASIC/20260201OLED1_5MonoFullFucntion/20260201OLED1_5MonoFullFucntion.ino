#include <Arduino.h>
#include <U8g2lib.h>

#ifdef U8X8_HAVE_HW_SPI
#include <SPI.h>
#endif
#ifdef U8X8_HAVE_HW_I2C
#include <Wire.h>
#endif

// === 硬體腳位定義 (ESP32-S3) ===
#define I2C_SDA_PIN 41
#define I2C_SCL_PIN 42

// === U8G2 建構函式 ===
// F 模式 (Full Framebuffer) 確保畫面不閃爍
U8G2_SSD1327_WS_128X128_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

void setup(void) {
  // 1. 初始化 I2C
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // 2. 初始化 OLED
  u8g2.begin();

  // 3. 關鍵設定：將對比度設為最大
  u8g2.setContrast(255);

  // 4. 設定字型為您電腦確定有的 u8x8 字體
  u8g2.setFont(u8x8_font_chroma48medium8_r);
}

void loop(void) {
  u8g2.clearBuffer();   // 1. 清空緩衝區

  // === 關鍵修正：針對 SSD1327 灰階螢幕 ===
  // 15 = 最亮 (白色), 0 = 黑色
  // 如果用 1，有些螢幕會顯示極暗的灰色導致看不見
  u8g2.setDrawColor(15);

  // 設定字體背景為透明 (避免黑色方塊擋住背景)
  u8g2.setFontMode(1);

  // --- 繪圖測試 ---
  u8g2.drawFrame(0, 0, 128, 128); // 邊框
  u8g2.drawLine(10, 45, 118, 45); // 分隔線

  // 畫一些幾何圖形證明繪圖正常
  u8g2.drawDisc(30, 80, 15);   // 實心圓
  u8g2.drawCircle(64, 80, 15); // 空心圓
  u8g2.drawBox(90, 65, 30, 30);// 實心矩形

  // --- 顯示文字 ---
  // 因為是 u8x8 字體，字比較小，我們用兩行來顯示
  u8g2.drawStr(10, 20, "SSD1327 OLED");
  u8g2.drawStr(10, 35, "Graphics Mode");

  // --- 動態數值 ---
  u8g2.drawStr(10, 115, "Time:");

  u8g2.setCursor(55, 115);
  u8g2.print(millis() / 1000.0);
  u8g2.print(" s");

  u8g2.sendBuffer(); // 送出顯示

  delay(100);
}
