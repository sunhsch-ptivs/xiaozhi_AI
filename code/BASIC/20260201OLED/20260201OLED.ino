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

//U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* clock=*/ SCL, /* data=*/ SDA, /* reset=*/ U8X8_PIN_NONE);  // High speed I2C
// U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, /* clock=*/ SCL, /* data=*/ SDA, /* reset=*/ U8X8_PIN_NONE);    //Low spped I2C

// === 硬體腳位定義 ===
#define I2C_SDA_PIN 41 // OLED SDA
#define I2C_SCL_PIN 42 // OLED SCL

void setup(void)
{
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN); // === 初始化 I2C 腳位 (SDA 41, SCL 42) ===
  u8g2.begin();
}

void loop(void)
{
  u8g2.clearBuffer();                   // clear the internal memory
  u8g2.setFont(u8g2_font_6x12_tf);   // choose a suitable font
  u8g2.drawStr(0, 10, "Hello World!");  // write something to the internal memory
  u8g2.sendBuffer();                    // transfer internal memory to the display
  delay(1000);
}
