/*
 * circle - ESP-NOW LED Circle Fixture
 * Jonas Vorwerk 2026
 *
 * ESP8266 + APA102 (ring / 1-D strip)
 * Data: GPIO 14  |  Clock: GPIO 12
 * Receives commands from M5Stack remote via ESP-NOW
 *
 * Modes (set via remote):
 *   0 - Solid color
 *   1 - Confetti
 *   2 - Noise flow (Perlin noise, fixed hue)
 *   3 - Noise flow with slowly drifting hue
 *   5 - Wave 1: sine brightness wave
 *   6 - Wave 2: sawtooth scanning dot
 *   7 - Wave 3: individual oscillation (each LED its own frequency)
 *   8 - Wave 4: full ring pulse
 *   9 - Move a Dot  ← default
 */

#include <ESP8266WiFi.h>
#include <espnow.h>
#include "FastLED.h"
#include <EEPROM.h>

#define EEPROM_MAGIC      0xAB
#define EEPROM_ADDR_MAGIC 0
#define EEPROM_ADDR_DATA  1

#define DEVICE_NAME        "Circle"

#define NUM_LEDS           300
#define DATA_PIN           14
#define CLOCK_PIN          12

#define BRIGHTNESS         200
#define FRAMES_PER_SECOND  60

CRGB leds[NUM_LEDS];

// Current state (set via ESP-NOW)
uint8_t hue     = 30;
uint8_t sat     = 200;
uint8_t val     = 200;
bool    power   = true;
int     mode    = 9;    // Move a Dot is default
uint8_t speed   = 128;
uint8_t fadeout = 10;

// Noise state
uint16_t noiseZ     = 0;
uint16_t noiseScale = 20;

// Hue drift state (mode 3)
uint8_t       driftHue          = 0;
unsigned long previousMillisHue = 0;

// Confetti state
unsigned long previousMillisConfetti = 0;

// Move a Dot state
#define NUM_DOTS              5
#define MAX_DOT_AGE           500
#define SPEED_DOTS_DIFFERENCE 50
#define HUE_SPREAD            8    // ±8 from base hue — nearby colours only
#define SAT_SPREAD            200  // dots can desaturate up to 200 below base (→ white)
#define VAL_SPREAD            40   // each dot dimmed 0–40 below val (keeps all dots in bright range)

float   pos_dot[NUM_DOTS];
float   spd_dot[NUM_DOTS];
int     age_dot[NUM_DOTS];
int8_t  hue_offset[NUM_DOTS];   // offset from base hue
uint8_t sat_reduction[NUM_DOTS]; // desaturates toward white (0 = base colour, high = white)
uint8_t val_reduction[NUM_DOTS]; // subtracted from val (0 = full brightness)
bool    dotsInitialized = false;

// ---------- PRESETS ----------
#define MAX_PRESETS 8

struct LampPreset {
  char    name[12];
  uint8_t h, s, v;
  bool    power;
  uint8_t mode;
  uint8_t speed;
  uint8_t fadeout;
};

typedef struct {
  bool       isPresetPacket;
  uint8_t    count;
  LampPreset slots[MAX_PRESETS];
} preset_packet;

static const LampPreset lampPresets[MAX_PRESETS] = {
  { "Warm",      18, 200, 255, true, 0, 128,  10 },  // Warm amber, solid
  { "Cool",     155, 130, 230, true, 0, 128,  10 },  // Cool blue-white, solid
  { "Vivid",      0, 255, 255, true, 0, 128,  10 },  // Saturated red, solid
  { "Ocean",    135, 255, 200, true, 0, 128,  10 },  // Deep teal, solid
  { "Confetti",  80, 255, 255, true, 1, 180,  80 },  // Green confetti
  { "Dots",      30, 200, 220, true, 9, 128,  10 },  // Move a Dot
  { "Wave",     170, 255, 220, true, 5, 150,  10 },  // Sine wave, blue
  { "Night",     20, 220,  60, true, 0, 128,  10 },  // Dim warm
};

// ---------- ESP-NOW MESSAGE STRUCT ----------
typedef struct struct_message {
  uint8_t h;
  uint8_t s;
  uint8_t v;
  bool power;
  int mode;
  uint8_t speed;
  uint8_t fadeout;
  bool requestState;   // If true: reply with current state, don't apply changes
  bool isDiscovery;    // If true: discovery broadcast — reply with device name
  char deviceName[16]; // Device name sent in discovery reply
  bool requestPresets; // If true: reply with preset list
} struct_message;

struct_message incomingData;

// ---------- ESP-NOW CALLBACK ----------
void onDataRecv(uint8_t * mac, uint8_t *incomingDataBytes, uint8_t len) {
  memcpy(&incomingData, incomingDataBytes, sizeof(incomingData));

  // If the remote is doing a discovery broadcast, reply with our name and return
  if (incomingData.isDiscovery) {
    struct_message response = {};
    response.isDiscovery = true;
    strncpy(response.deviceName, DEVICE_NAME, sizeof(response.deviceName) - 1);
    esp_now_add_peer(mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
    esp_now_send(mac, (uint8_t*)&response, sizeof(response));
    Serial.println("Discovery request received — sending name back to remote");
    return;
  }

  // If the remote is requesting our state, send it back and return
  if (incomingData.requestState) {
    struct_message response = {};  // zero-init: ensures isDiscovery=false, deviceName=""
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

  // If the remote is requesting our presets, send them and return
  if (incomingData.requestPresets) {
    preset_packet pp = {};
    pp.isPresetPacket = true;
    pp.count = MAX_PRESETS;
    for (int i = 0; i < MAX_PRESETS; i++) pp.slots[i] = lampPresets[i];
    esp_now_add_peer(mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
    esp_now_send(mac, (uint8_t*)&pp, sizeof(pp));
    Serial.println("Preset request received — sending presets to remote");
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

  Serial.print("ESP-NOW -> H:");
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
  Serial.println("circle booting...");

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

  WiFi.mode(WIFI_STA);
  Serial.print("MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != 0) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onDataRecv);
  Serial.println("ESP-NOW Ready!");

  FastLED.addLeds<APA102, DATA_PIN, CLOCK_PIN, BGR>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear();

  noiseZ = random16();
}

// ---------- LOOP ----------
void loop() {
  displayLeds();
  FastLED.show();
  FastLED.delay(1000 / FRAMES_PER_SECOND);
}

// ---------- SOLID COLOR ----------
void solidColor() {
  fill_solid(leds, NUM_LEDS, CHSV(hue, sat, val));
}

// ---------- CONFETTI ----------
void confetti() {
  uint8_t fadeAmount = map(fadeout, 0, 255, 2, 50);
  fadeToBlackBy(leds, NUM_LEDS, fadeAmount);

  unsigned long now           = millis();
  unsigned long spawnInterval = map(speed, 0, 255, 150, 16);

  if (now - previousMillisConfetti >= spawnInterval) {
    previousMillisConfetti = now;
    int pos = random16(NUM_LEDS);
    leds[pos] += CHSV(hue + random8(64), sat, val);
  }
}

// ---------- NOISE FLOW (1D) ----------
void noiseFlow() {
  uint8_t noiseSpeed = map(speed, 0, 255, 1, 8);
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t n          = inoise8(i * noiseScale, noiseZ);
    uint8_t brightness = map(n, 0, 255, 0, val);
    leds[i] = CHSV(hue, sat, brightness);
  }
  noiseZ += noiseSpeed;
}

// ---------- NOISE FLOW HUE DRIFT (1D) ----------
void noiseFlowHue() {
  uint8_t noiseSpeed = map(speed, 0, 255, 1, 8);

  unsigned long now = millis();
  if (now - previousMillisHue >= 500) {
    previousMillisHue = now;
    driftHue++;
  }

  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t n          = inoise8(i * noiseScale, noiseZ);
    uint8_t brightness = map(n, 0, 255, 0, val);
    leds[i] = CHSV(driftHue, sat, brightness);
  }
  noiseZ += noiseSpeed;
}

// ---------- WAVE HELPERS ----------
uint8_t getBpm() {
  return map(speed, 0, 255, 5, 60);
}

// Wave 1: sine brightness wave across the ring
void wave1() {
  uint8_t bpm = getBpm();
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t phase      = (uint8_t)map(i, 0, NUM_LEDS - 1, 0, 255);
    uint8_t brightness = beatsin8(bpm, 0, val, 0, phase);
    leds[i] = CHSV(hue, sat, brightness);
  }
}

// Wave 2: sawtooth scanning dot
void wave2() {
  fadeToBlackBy(leds, NUM_LEDS, map(fadeout, 0, 255, 2, 50));
  uint8_t bpm = getBpm();
  int pos = map8(beat8(bpm), 0, NUM_LEDS - 1);
  leds[pos] = CHSV(hue, sat, val);
}

// Wave 3: each LED oscillates at its own slightly different frequency
void wave3() {
  uint8_t bpm = getBpm();
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t brightness = beatsin8(bpm + i, 0, val);
    leds[i] = CHSV(hue, sat, brightness);
  }
}

// Wave 4: full ring breathes together
void wave4() {
  uint8_t bpm        = getBpm();
  uint8_t brightness = beatsin8(bpm, 0, val);
  fill_solid(leds, NUM_LEDS, CHSV(hue, sat, brightness));
}

// ---------- MOVE A DOT ----------
void initDot(int i) {
  int globalSpeed = map(speed, 0, 255, 1, 60);                          // 1–60 (÷100 → 0.01–0.60 LEDs/frame)
  int spread      = map(speed, 0, 255, 3, SPEED_DOTS_DIFFERENCE);       // spread also scales with speed
  int minSpd      = globalSpeed;
  int maxSpd      = constrain(globalSpeed + spread, 0, 100);
  spd_dot[i]    = (float)map(random(100), 0, 100, minSpd, maxSpd) / 100.0f
                  * (random(2) * 2 - 1);           // random ± direction
  pos_dot[i]       = random(NUM_LEDS);
  age_dot[i]       = random(MAX_DOT_AGE / 10);
  hue_offset[i]    = (int8_t)(random(HUE_SPREAD * 2 + 1) - HUE_SPREAD);   // ±8 hue
  sat_reduction[i] = random8(SAT_SPREAD + 1);                              // 0–200 toward white
  val_reduction[i] = random8(VAL_SPREAD + 1);                              // 0–40 dimmer than val
}

void initAllDots() {
  for (int i = 0; i < NUM_DOTS; i++) initDot(i);
  dotsInitialized = true;
}

void moveADot() {
  if (!dotsInitialized) initAllDots();

  EVERY_N_MILLISECONDS(50) {
    if (fadeout > 0) {
      fadeToBlackBy(leds, NUM_LEDS, map(fadeout, 1, 255, 2, 50));
    }
  }

  for (int i = 0; i < NUM_DOTS; i++) {
    // Draw dot — all three channels vary slightly around the base values
    uint8_t dotSat = (uint8_t)max(0, (int)sat - (int)sat_reduction[i]);
    uint8_t dotVal = (uint8_t)max(0, (int)val - (int)val_reduction[i]);
    leds[(int)pos_dot[i]] = CHSV((uint8_t)(hue + hue_offset[i]), dotSat, dotVal);

    // Move dot
    pos_dot[i] += spd_dot[i];

    // Age dot and reinitialise when expired
    age_dot[i]++;
    if (age_dot[i] > MAX_DOT_AGE) initDot(i);

    // Wrap around the ring
    if (pos_dot[i] >= NUM_LEDS && spd_dot[i] > 0) pos_dot[i] = 0;
    if (pos_dot[i] < 0         && spd_dot[i] < 0) pos_dot[i] = NUM_LEDS - 1;
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
      solidColor();
      break;
    case 1:
      confetti();
      break;
    case 2:
      noiseFlow();
      break;
    case 3:
      noiseFlowHue();
      break;
    case 5:
      wave1();
      break;
    case 6:
      wave2();
      break;
    case 7:
      wave3();
      break;
    case 8:
      wave4();
      break;
    case 9:
    default:
      moveADot();
      break;
  }
}
