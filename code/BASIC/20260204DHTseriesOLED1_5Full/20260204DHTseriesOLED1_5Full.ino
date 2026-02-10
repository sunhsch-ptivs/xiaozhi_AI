#include <Arduino.h>
#include <U8g2lib.h>
#include <DHT.h>
#include <DHT_U.h>

#ifdef U8X8_HAVE_HW_SPI
#include <SPI.h>
#endif
#ifdef U8X8_HAVE_HW_I2C
#include <Wire.h>
#endif

// === 硬體腳位定義 (ESP32-S3) ===
#define I2C_SDA_PIN 41
#define I2C_SCL_PIN 42
#define DHTPIN 11          // DHT22 數據腳位接在 GPIO 11
#define DHTTYPE DHT11     // 定義感測器型號為 DHT11

// === 初始化物件 ===
// 使用 Full Framebuffer 模式確保灰階顯示穩定
U8G2_SSD1327_WS_128X128_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
DHT dht(DHTPIN, DHTTYPE);

void setup(void) {
  // 1. 初始化 I2C 與序列埠
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Serial.begin(115200);

  // 2. 初始化 DHT 感測器
  dht.begin();

  // 3. 初始化 OLED
  u8g2.begin();
  u8g2.setContrast(255); // 設為最大亮度
}

void loop(void) {
  // 讀取溫溼度
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  u8g2.clearBuffer();
  u8g2.setDrawColor(15); // 灰階最大值：白色
  u8g2.setFontMode(1);   // 透明背景模式

  // --- 繪製外框 ---
  u8g2.drawFrame(0, 0, 128, 128);

  // 標題區：使用簡潔的字體
  u8g2.setFont(u8g2_font_6x13_tr);
  u8g2.drawStr(25, 20, "ENV MONITOR");
  u8g2.drawLine(10, 25, 118, 25);

  // 檢查讀取是否正常
  if (isnan(h) || isnan(t)) {
    u8g2.drawStr(20, 65, "Sensor Error!");
  } else {
    // --- 溫度顯示 ---
    // 使用 Helvetica 粗體 12 像素，間距較穩定的字體
    u8g2.setFont(u8g2_font_helvB12_tr);


    u8g2.drawStr(10, 55, "Tem:");
    u8g2.drawStr(55, 55, String(t).c_str()); // 顯示到小數點第二位
    Serial.println(t);
    u8g2.drawStr(105, 55, "C"); // 顯示到小數點第二位

    // --- 濕度顯示 ---
    u8g2.drawStr(10, 85, "Hum:");
    u8g2.drawStr(55, 85, String(h).c_str()); // 顯示到小數點第二位
    Serial.println(h);
    u8g2.drawStr(105, 85, "%"); // 顯示到小數點第二位
  }

  // --- 底部裝飾線與圖形 ---
  u8g2.drawLine(10, 100, 118, 100);
  u8g2.drawDisc(35, 114, 8);
  u8g2.drawCircle(64, 114, 8);
  u8g2.drawBox(90, 106, 16, 16);

  u8g2.sendBuffer();
  delay(1000); // DHT22 建議讀取間隔為 2 秒
}
