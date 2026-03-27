/*
 * Vibration motor test for M5Stack Core2
 * Tries multiple methods and shows result on screen.
 */

#include <M5Unified.h>

static inline uint8_t axpRead(uint8_t reg) {
  return M5.In_I2C.readRegister8(0x34, reg, 400000UL);
}
static inline void axpWrite(uint8_t reg, uint8_t val) {
  M5.In_I2C.writeRegister8(0x34, reg, val, 400000UL);
}

// Method A: direct AXP192 LDO3 via register 0x28 / 0x12
void vibrateA(uint16_t ms) {
  uint8_t ldo = axpRead(0x28);
  axpWrite(0x28, (ldo & 0xF0) | 0x0C);  // LDO3 = 3.0V
  axpWrite(0x12, axpRead(0x12) | 0x08); // enable LDO3
  delay(ms);
  axpWrite(0x12, axpRead(0x12) & ~0x08);
}

// Method B: M5.Power.setVibration()
void vibrateB(uint16_t ms) {
  M5.Power.setVibration(200);
  delay(ms);
  M5.Power.setVibration(0);
}

// Method C: LDO3 at full 3.3V
void vibrateC(uint16_t ms) {
  uint8_t ldo = axpRead(0x28);
  axpWrite(0x28, (ldo & 0xF0) | 0x0F);  // LDO3 = 3.3V (max)
  axpWrite(0x12, axpRead(0x12) | 0x08);
  delay(ms);
  axpWrite(0x12, axpRead(0x12) & ~0x08);
}

void drawButton(const char* label, int y, uint16_t color) {
  M5.Display.fillRoundRect(20, y, 280, 55, 8, color);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(30, y + 18);
  M5.Display.print(label);
}

void drawStatus(const char* msg) {
  M5.Display.fillRect(0, 215, 320, 25, TFT_BLACK);
  M5.Display.setTextColor(TFT_YELLOW);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(5, 220);
  M5.Display.print(msg);
}

void setup() {
  M5.begin();
  M5.Display.setRotation(1);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(60, 5);
  M5.Display.print("Vibration Test");

  drawButton("A: LDO3 direct (3.0V)", 40,  0x0390);
  drawButton("B: setVibration(200)",   105, 0x6200);
  drawButton("C: LDO3 direct (3.3V)", 170,  0x000F);

  Serial.begin(115200);
  Serial.println("Vibration test ready");
  Serial.printf("AXP reg 0x12=0x%02X  0x28=0x%02X\n", axpRead(0x12), axpRead(0x28));
}

void loop() {
  M5.update();

  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) {
    int tx = t.x, ty = t.y;

    if (ty >= 40 && ty <= 95) {
      drawStatus("Method A: LDO3 3.0V direct...");
      Serial.println("Testing method A");
      vibrateA(115); delay(10); vibrateA(15);
      drawStatus("Done - did you feel it?");
    }
    else if (ty >= 105 && ty <= 160) {
      drawStatus("Method B: setVibration(200)...");
      Serial.println("Testing method B");
      vibrateB(100); delay(10); vibrateB(15);
      drawStatus("Done - did you feel it?");
    }
    else if (ty >= 170 && ty <= 225) {
      drawStatus("Method C: LDO3 3.3V direct...");
      Serial.println("Testing method C");
      vibrateC(115); delay(10); vibrateC(15);
      drawStatus("Done - did you feel it?");
    }
  }
}
