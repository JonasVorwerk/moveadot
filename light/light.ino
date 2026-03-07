#include <ESP8266WiFi.h>
#include <espnow.h>
#include "FastLED.h"
#include <EEPROM.h>

#define EEPROM_MAGIC 0xAB
#define EEPROM_ADDR_MAGIC 0
#define EEPROM_ADDR_DATA  1

#define DEVICE_NAME "Light"


#define NUM_LEDS    180
#define BRIGHTNESS  128
#define FRAMES_PER_SECOND  30

CRGB leds[NUM_LEDS];

// Current color (set via ESP-NOW)
uint8_t hue = 30;
uint8_t sat = 200;
uint8_t val = 200;
bool power = true;
int mode = 1;
uint8_t speed = 128;   // Animation speed (0-255)
uint8_t fadeout = 10;  // Fadeout rate (0-255)

// Shooting star variables
int current_led = 0;
bool direction = true;  // true = up, false = down
unsigned long lastStarChange = 0;
unsigned long starChangeInterval = 15000;  // milliseconds

// ---------- ESP-NOW MESSAGE STRUCT ----------
typedef struct struct_message {
  uint8_t h;
  uint8_t s;
  uint8_t v;
  bool power;
  int mode;
  uint8_t speed;
  uint8_t fadeout;
  bool requestState;  // If true: reply with current state, don't apply changes
} struct_message;

struct_message incomingData;

// ---------- ESP-NOW CALLBACK ----------
void onDataRecv(uint8_t * mac, uint8_t *incomingDataBytes, uint8_t len) {
  memcpy(&incomingData, incomingDataBytes, sizeof(incomingData));

  // If the remote is requesting our state, send it back and return
  if (incomingData.requestState) {
    struct_message response;
    response.h            = hue;
    response.s            = sat;
    response.v            = val;
    response.power        = power;
    response.mode         = mode;
    response.speed        = speed;
    response.fadeout      = fadeout;
    response.requestState = false;
    esp_now_add_peer(mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
    esp_now_send(mac, (uint8_t*)&response, sizeof(response));
    Serial.println("State request received — sending state back to remote");
    return;
  }

  hue     = incomingData.h;
  sat     = incomingData.s;
  val     = incomingData.v;
  power   = incomingData.power;
  mode    = incomingData.mode;
  speed   = incomingData.speed;
  fadeout = incomingData.fadeout;

  // Save settings to EEPROM so they survive a power cycle
  EEPROM.write(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
  EEPROM.put(EEPROM_ADDR_DATA, incomingData);
  EEPROM.commit();

  Serial.print("ESP-NOW Data Received -> H:");
  Serial.print(hue);
  Serial.print(" S:");
  Serial.print(sat);
  Serial.print(" V:");
  Serial.print(val);
  Serial.print(" Power:");
  Serial.print(power);
  Serial.print(" Mode:");
  Serial.print(mode);
  Serial.print(" Speed:");
  Serial.print(speed);
  Serial.print(" Fade:");
  Serial.println(fadeout);
}

// ---------- SETUP ----------
void setup() {
  delay(2000);
  Serial.begin(115200);
  Serial.println("Booting...");

  // --- EEPROM: load saved settings ---
  EEPROM.begin(1 + sizeof(struct_message));
  if (EEPROM.read(EEPROM_ADDR_MAGIC) == EEPROM_MAGIC) {
    EEPROM.get(EEPROM_ADDR_DATA, incomingData);
    hue     = incomingData.h;
    sat     = incomingData.s;
    val     = incomingData.v;
    power   = incomingData.power;
    mode    = incomingData.mode;
    speed   = incomingData.speed;
    fadeout = incomingData.fadeout;
    Serial.println("Settings restored from EEPROM.");
  } else {
    Serial.println("No saved settings found, using defaults.");
  }

  // --- WIFI STATION MODE ---
  WiFi.mode(WIFI_STA);

  Serial.print("MAC address: ");
  Serial.println(WiFi.macAddress());

  // --- ESP-NOW INIT ---
  if (esp_now_init() != 0) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onDataRecv);
  Serial.println("ESP-NOW Ready!");

  // --- LED SETUP ---
  FastLED.addLeds<WS2812B, 14, RGB>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  //FastLED.addLeds<APA102, 14, 12, BGR>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear();
}

// ---------- SERIAL CONTROL ----------
void checkSerial() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();

  if (cmd == "GET") {
    Serial.print("STATE:");
    Serial.print(hue);     Serial.print(",");
    Serial.print(sat);     Serial.print(",");
    Serial.print(val);     Serial.print(",");
    Serial.print(power);   Serial.print(",");
    Serial.print(mode);    Serial.print(",");
    Serial.print(speed);   Serial.print(",");
    Serial.println(fadeout);
    return;
  }

  if (cmd == "NAME") {
    Serial.println("NAME:" DEVICE_NAME);
    return;
  }

  // Parse: h,s,v,power,mode,speed,fadeout
  int vals[7];
  int idx = 0;
  char buf[64];
  cmd.toCharArray(buf, sizeof(buf));
  char* tok = strtok(buf, ",");
  while (tok && idx < 7) { vals[idx++] = atoi(tok); tok = strtok(NULL, ","); }
  if (idx == 7) {
    hue     = (uint8_t)vals[0];
    sat     = (uint8_t)vals[1];
    val     = (uint8_t)vals[2];
    power   = (bool)vals[3];
    mode    = vals[4];
    speed   = (uint8_t)vals[5];
    fadeout = (uint8_t)vals[6];
    EEPROM.write(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
    struct_message save = {hue, sat, val, power, mode, speed, fadeout};
    EEPROM.put(EEPROM_ADDR_DATA, save);
    EEPROM.commit();
    Serial.println("OK");
  }
}

// ---------- LOOP ----------
void loop() {
  checkSerial();
  displayLeds();
  FastLED.show();
  FastLED.delay(1000 / FRAMES_PER_SECOND);
}

// ---------- CONFETTI ----------
void confetti() {
  // Random colored speckles that fade smoothly
  // Fadeout controls fade rate directly (0=no fade, 255=instant fade)
  uint8_t fadeAmount = map(fadeout, 0, 255, 2, 50);  // Map to reasonable fade range
  fadeToBlackBy(leds, NUM_LEDS, fadeAmount);
  
  // Speed controls spawn rate (how many sparkles per frame)
  int spawnCount = map(speed, 0, 255, 1, 5);  // 1 to 5 sparkles per frame
  for (int i = 0; i < spawnCount; i++) {
    int pos = random16(NUM_LEDS);
    leds[pos] += CHSV(hue + random8(64), sat, val);
  }
}

// ---------- SHOOTING STAR ----------
void shootingStar() {
  // Single LED travels up or down the strip with trailing fade
  
  // Light up current LED
  leds[current_led] = CHSV(hue, sat, val);
  
  // Move LED position based on direction
  if (direction) {
    // Moving up
    if (current_led < NUM_LEDS - 1) {
      current_led++;
    }
  } else {
    // Moving down
    if (current_led > 0) {
      current_led--;
    }
  }
  
  // Fade all LEDs (create trailing effect)
  uint8_t fadeAmount = map(fadeout, 0, 255, 2, 50);
  fadeToBlackBy(leds, NUM_LEDS, fadeAmount);
  
  // Check if it's time to change direction and reset
  unsigned long now = millis();
  if (now - lastStarChange >= starChangeInterval) {
    // Change direction (random)
    direction = random(0, 2);
    
    // Reset position based on direction
    if (direction) {
      current_led = 0;  // Start at bottom, move up
    } else {
      current_led = NUM_LEDS - 1;  // Start at top, move down
    }
    
    // Set next change interval (speed controls timing)
    starChangeInterval = map(speed, 0, 255, 30000, 5000);  // 30s to 5s
    lastStarChange = now;
  }
}

// ---------- DISPLAY LEDS ----------
void displayLeds() {
  if (!power) {
    FastLED.clear();
    return;
  }

  switch (mode) {
    case 0:
    default:
      // Solid color
      fill_solid(leds, NUM_LEDS, CHSV(hue, sat, val));
      break;

    case 1:
      // Confetti
      confetti();
      break;

    case 2:
      // Shooting Star
      shootingStar();
      break;
  }
}
