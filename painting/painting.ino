/*
 * painting - ESP-NOW LED Painting Fixture
 * Jonas Vorwerk 2026
 *
 * ESP8266 + APA102 (COLS x ROWS matrix)
 * Data: GPIO 12  |  Clock: GPIO 14
 * Receives commands from M5Stack remote via ESP-NOW
 *
 * Modes (set via remote):
 *   0 - Solid color
 *   1 - Confetti
 *   2 - Noise flow (2D Perlin noise, fixed hue)
 *   3 - Noise flow with slowly drifting hue  ← default
 *   4 - Weirdo (autonomous random color/effect changes)
 *   5 - Wave 1: sine scanner (beatsin8 dot per column)
 *   6 - Wave 2: sawtooth scanner (beat8 dot per row)
 *   7 - Wave 3: individual oscillation (each LED its own frequency)
 *   8 - Wave 4: row pulse (each row breathes at its own rate)
 */

#include <ESP8266WiFi.h>
#include <espnow.h>
#include "FastLED.h"
#include <EEPROM.h>
#include <OneButton.h>

#define BUTTON_PIN        15   // GPIO15 — needs external pull-down (built-in on most ESP8266 boards)

#define EEPROM_MAGIC      0xAB
#define EEPROM_ADDR_MAGIC 0
#define EEPROM_ADDR_DATA  1

//Painting Big Nico
// #define ROWS               14
// #define COLS               7

#define DEVICE_NAME        "Painting"

//Painting test Jonas
#define ROWS               15
#define COLS               5

#define NUM_LEDS           (ROWS * COLS)
#define SERPENTINE         1               // 1 = serpentine wiring, 0 = parallel

#define DATA_PIN           14
#define CLOCK_PIN          12

#define BRIGHTNESS         200
#define FRAMES_PER_SECOND  60

CRGB leds[NUM_LEDS];

// Button — active HIGH (GPIO15 has built-in pull-down, button connects to 3.3V)
OneButton btn(BUTTON_PIN, false, false);

// Current state (set via ESP-NOW)
uint8_t hue     = 30;
uint8_t sat     = 200;
uint8_t val     = 200;
bool    power   = true;
int     mode    = 3;
uint8_t speed   = 128;
uint8_t fadeout = 10;

// Noise state
uint16_t noiseZ     = 0;
uint16_t noiseScale = 20;

// Hue drift state (mode 3)
uint8_t       driftHue          = 0;
unsigned long previousMillisHue = 0;  // Timer: hue increments every 500ms (~2 min full cycle)

// Confetti state
unsigned long previousMillisConfetti = 0;

// Weirdo state (autonomous, not controlled by remote)
unsigned long previousMillisWeirdo = 0;
unsigned long previousMillisStep   = 0;  // Throttle for scroll steps
uint8_t weirdoHue       = 20;
uint8_t weirdoSat       = 125;
uint8_t weirdoFadeout   = 50;
uint8_t weirdoI         = 0;     // Column counter used by scroll
bool    weirdoDirection = true;  // true = scroll up, false = scroll down

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
  { "Warm",      18, 180, 255, true, 0, 128,  10 },  // Warm amber, solid
  { "Daylight", 150,  80, 255, true, 0, 128,  10 },  // Cool daylight, solid
  { "Sunset",    15, 255, 200, true, 0, 128,  10 },  // Deep orange, solid
  { "Weirdo",    80, 200, 200, true, 4, 128,  50 },  // Autonomous weirdo
  { "Noise",     30, 200, 200, true, 3, 100,  20 },  // Noise hue drift
  { "Wave",     170, 255, 220, true, 5, 150,  20 },  // Sine wave, blue
  { "Night",     20, 220,  50, true, 0, 128,  10 },  // Dim warm
  { "Confetti",  80, 255, 255, true, 1, 180,  80 },  // Confetti
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

// ---------- BUTTON HANDLER ----------
void handleButtonClick() {
  mode = (mode + 1) % 9;  // Cycle through modes 0-8
  incomingData.mode = mode;
  EEPROM.write(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
  EEPROM.put(EEPROM_ADDR_DATA, incomingData);
  EEPROM.commit();
  Serial.print("Button: mode -> ");
  Serial.println(mode);
}

// ---------- SETUP ----------
void setup() {
  delay(2000);
  Serial.begin(115200);
  Serial.println("painting booting...");

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

  //APA102
  FastLED.addLeds<APA102, DATA_PIN, CLOCK_PIN, BGR>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);

  //Painting Nico Big
  //FastLED.addLeds<UCS2903,14,RGB>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(BRIGHTNESS);
  FastLED.clear();

  noiseZ    = random16();
  weirdoHue = random(256);

  btn.attachClick(handleButtonClick);
}

// ---------- LOOP ----------
void loop() {
  btn.tick();
  displayLeds();
  FastLED.show();
  FastLED.delay(1000 / FRAMES_PER_SECOND);
}

// ---------- XY MATRIX MAPPING ----------
uint16_t XY(int x, int y) {
  if (SERPENTINE && (y & 0x01)) {
    return (y * COLS) + (COLS - 1 - x);  // Reversed row
  }
  return (y * COLS) + x;
}

// ---------- SOLID COLOR ----------
void solidColor() {
  fill_solid(leds, NUM_LEDS, CHSV(hue, sat, val));
}

// ---------- CONFETTI ----------
// Fade runs every frame for smooth decay.
// Spawning is rate-limited: speed maps to interval (slow: 150ms, fast: 16ms).
// One pixel per spawn for a fluid, even stream rather than bursts.
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

// ---------- NOISE FLOW ----------
// 2D Perlin noise across the matrix. Faithful to noiseFx in the original painting.
void noiseFlow() {
  uint8_t noiseSpeed = map(speed, 0, 255, 1, 8);

  for (int y = 0; y < ROWS; y++) {
    for (int x = 0; x < COLS; x++) {
      uint8_t n = inoise8(x * noiseScale, y * noiseScale, noiseZ);
      uint8_t brightness = map(n, 0, 255, 0, val);
      leds[XY(x, y)] = CHSV(hue, sat, brightness);
    }
  }

  noiseZ += noiseSpeed;
}

// ---------- NOISE FLOW HUE DRIFT ----------
// Same 2D Perlin noise as noiseFlow, but hue drifts autonomously through
// the full spectrum — one step every 500ms (~2 minute full rainbow cycle).
void noiseFlowHue() {
  uint8_t noiseSpeed = map(speed, 0, 255, 1, 8);

  unsigned long now = millis();
  if (now - previousMillisHue >= 500) {
    previousMillisHue = now;
    driftHue++;
  }

  for (int y = 0; y < ROWS; y++) {
    for (int x = 0; x < COLS; x++) {
      uint8_t n = inoise8(x * noiseScale, y * noiseScale, noiseZ);
      uint8_t brightness = map(n, 0, 255, 0, val);
      leds[XY(x, y)] = CHSV(driftHue, sat, brightness);
    }
  }

  noiseZ += noiseSpeed;
}

// ==========================================================
// WEIRDO SUB-EFFECTS (matrix adaptations of painting effects)
// weirdoHue/weirdoSat/weirdoFadeout are autonomous.
// val (brightness) is always from the remote.
// ==========================================================

// Scroll: shift all rows up or down and plot next column pixel at the leading edge.
// Direction is controlled by weirdoDirection (true = up, false = down).
void scroll() {
  if (weirdoDirection) {
    // Shift rows up
    for (int y = 0; y < ROWS - 1; y++) {
      for (int x = 0; x < COLS; x++) {
        leds[XY(x, y)] = leds[XY(x, y + 1)];
      }
    }
    for (int x = 0; x < COLS; x++) {
      leds[XY(x, ROWS - 1)].fadeToBlackBy(weirdoFadeout);
    }
    leds[XY(weirdoI % COLS, ROWS - 1)] = CHSV(weirdoHue, weirdoSat, val);
  } else {
    // Shift rows down
    for (int y = ROWS - 1; y > 0; y--) {
      for (int x = 0; x < COLS; x++) {
        leds[XY(x, y)] = leds[XY(x, y - 1)];
      }
    }
    for (int x = 0; x < COLS; x++) {
      leds[XY(x, 0)].fadeToBlackBy(weirdoFadeout);
    }
    leds[XY(weirdoI % COLS, 0)] = CHSV(weirdoHue, weirdoSat, val);
  }
  weirdoI++;
}


// ---------- WEIRDO ----------
// Always scrolls (like schuin in the original) with occasional color/fade changes.
// Faithful adaptation of weirdo() from the original painting.
// Speed slider controls scroll step rate (slow → fast: 200ms → 20ms per step).
// Color changes every 300-2100ms — 3x the original (100-700ms) to compensate
// for the smaller matrix (5 rows vs 15), keeping the same visual trail feel.
void weirdo() {
  unsigned long now          = millis();
  unsigned long stepInterval = map(speed, 0, 255, 200, 20);

  if (now - previousMillisStep >= stepInterval) {
    previousMillisStep = now;
    scroll();
  }

  if ((now - previousMillisWeirdo) >= (unsigned long)random(300, 2100)) {
    previousMillisWeirdo = now;
    weirdoHue     = random(256);
    weirdoSat     = random(25, 256);
    weirdoFadeout = random(10, 150);
    if (random(3) == 0) weirdoDirection = !weirdoDirection;  // ~33% chance per change — matches original flip rate
  }
}


// ==========================================================
// WAVE EFFECTS (matrix adaptations of wave1-4 from painting)
// speed maps to bpm (5-60), fadeout controls trail length.
// ==========================================================

uint8_t getBpm() {
  return map(speed, 0, 255, 5, 60);
}

// Wave 1: for each column, a dot at beatsin8 row position — forms a sine wave.
// Faithful matrix adaptation of wave1 from the painting.
void wave1() {
  fadeToBlackBy(leds, NUM_LEDS, map(fadeout, 0, 255, 2, 50));
  uint8_t bpm = getBpm();
  for (int x = 0; x < COLS; x++) {
    int row = beatsin8(x + bpm, 0, ROWS - 1);
    leds[XY(x, row)] = CHSV(hue, sat, val);
  }
}

// Wave 2: for each row, a dot at beat8 column position — sawtooth scanner.
// Faithful matrix adaptation of wave2 from the painting.
void wave2() {
  fadeToBlackBy(leds, NUM_LEDS, map(fadeout, 0, 255, 2, 50));
  uint8_t bpm = getBpm();
  for (int y = 0; y < ROWS; y++) {
    int col = map8(beat8(bpm + y), 0, COLS - 1);
    leds[XY(col, y)] = CHSV(hue, sat, val);
  }
}

// Wave 3: each LED oscillates at its own slightly different frequency.
// Faithful adaptation of wave3 from the painting (works linearly).
void wave3() {
  uint8_t bpm = getBpm();
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t brightness = beatsin8(bpm + i, 0, val);
    leds[i] = CHSV(hue, sat, brightness);
  }
}

// Wave 4: each row pulses as a whole at its own rate — like rows of light.
// Faithful matrix adaptation of wave4 from the painting.
void wave4() {
  uint8_t bpm = getBpm();
  for (int y = 0; y < ROWS; y++) {
    uint8_t brightness = beatsin8(bpm + y, 0, val);
    for (int x = 0; x < COLS; x++) {
      leds[XY(x, y)] = CHSV(hue, sat, brightness);
    }
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
    case 4:
      weirdo();
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
  }
}
