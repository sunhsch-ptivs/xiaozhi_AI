#include <ESP32Servo.h>

Servo myServo;
const int servoPin = 21;

void setup() {
  Serial.begin(115200);

  // ESP32 資源分配
  ESP32PWM::allocateTimer(0);

  Serial.println("請輸入角度 (0-180)，轉動後會自動釋放 (detach)：");
}

void loop() {
  if (Serial.available() > 0) {
    int angle = Serial.parseInt();

    if (angle >= 0 && angle <= 180) {
      Serial.printf("目標角度: %d 度，正在連接並轉動...\n", angle);

      // 1. 重新連接引腳
      myServo.attach(servoPin, 500, 2400);

      // 2. 寫入角度
      myServo.write(angle);

      // 3. 給予足夠時間讓馬達轉動 (MG90 轉 180 度約需 0.5~1 秒)
      delay(800);

      // 4. 斷開訊號以省電
      myServo.detach();

      Serial.println("已到達位置，訊號已切斷 (Detach OK)。");
    }

    // 清除緩存
    while (Serial.available() > 0) Serial.read();
  }
}
