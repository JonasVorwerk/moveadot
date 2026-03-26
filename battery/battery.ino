/*
 * battery.ino
 * Shows battery percentage, updates every second.
 * Powers off after SLEEP_DELAY ms when not on USB/charging.
 * Wake: power button only.
 */

#include <M5Unified.h>

#define SLEEP_DELAY 20000  // ms before power off
#define UPDATE_INTERVAL 1000  // ms between percentage updates

unsigned long startTime;
unsigned long lastUpdate = 0;
int lastLevel = -1;

void drawBattery(int level) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", level);

  M5.Display.setFont(&fonts::FreeSansBold24pt7b);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.drawString(buf, M5.Display.width() / 2, M5.Display.height() / 2);
  // M5.Display.setFont(&fonts::FreeSans9pt7b);
  // M5.Display.drawString("Battery", M5.Display.width() / 2, M5.Display.height() / 2 + 35);
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Display.setRotation(1);
  M5.Display.setBrightness(128);
  M5.Display.fillScreen(TFT_BLACK);

  startTime = millis();

  drawBattery(M5.Power.getBatteryLevel());
  lastLevel = M5.Power.getBatteryLevel();
}

void loop() {
  unsigned long now = millis();

  // Update percentage every second
  if (now - lastUpdate >= UPDATE_INTERVAL) {
    lastUpdate = now;
    int level = M5.Power.getBatteryLevel();
    if (level != lastLevel) {
      lastLevel = level;
      drawBattery(level);
    }
  }

  // Power off after delay if not on USB/charging
  if (now - startTime >= SLEEP_DELAY) {
    if (!M5.Power.isCharging() && M5.Power.getBatteryLevel() < 100) {
      M5.Power.powerOff();
    }
  }
}
