#include <ESP32Servo.h>

Servo myServo;
const int servoPin = 21; // 請確認接線與此引腳一致

void setup() {
  Serial.begin(115200);

  // 分配 ESP32 定時器資源
  ESP32PWM::allocateTimer(0);

  Serial.println("=== 伺服馬達序列埠控制程式 ===");
  Serial.println("請輸入目標角度 (0-180)，輸入後馬達將自動轉動並釋放訊號。");
}

void loop() {
  if (Serial.available() > 0) {
    // 讀取 Serial Monitor 輸入的整數角度
    int angle = Serial.parseInt();

    // 檢查角度是否在合法範圍內 (0-180度)
    if (angle >= 0 && angle <= 180) {
      Serial.print("正在轉動至： ");
      Serial.print(angle);
      Serial.println(" 度...");

      // 1. 重新連接引腳 (設定脈衝寬度範圍：500us - 2400us)
      myServo.attach(servoPin, 500, 2400);

      // 2. 寫入目標角度
      myServo.write(angle);

      // 3. 延遲 800ms 確保馬達有足夠時間到達指定位置
      delay(800);

      // 4. 到達位置後斷開訊號，防止抖動與省電
      myServo.detach();

      Serial.println("到達位置，已自動離合 (Detach OK)。");
    } else {
      // 排除非角度數字的錯誤輸入 (例如換行符號)
      if (angle != 0 || Serial.peek() != '\n') {
        Serial.println("錯誤：請輸入 0 到 180 之間的數值。");
      }
    }

    // 清除緩存區，避免多餘字元影響下次輸入
    while (Serial.available() > 0) Serial.read();
  }
}
