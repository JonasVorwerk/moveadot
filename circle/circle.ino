/*
 * Circle - ESP-NOW LED Circle Fixture
 * Jonas Vorwerk 2026
 *
 * ESP8266 + APA102 (300 LED ring)
 * Data: GPIO 14  |  Clock: GPIO 12
 * Receives commands from M5Stack remote via ESP-NOW
 *
 * Modes (set via remote):
 *   0  - Solid color
 *   1  - Confetti          (speed = spawn rate, fadeout = fade depth)
 *   2  - Noise flow        (Perlin noise, fixed hue)
 *   3  - Noise flow hue    (Perlin noise, drifting hue)
 *   4  - Half circle       (rotating split, speed = direction & rate)
 *   5  - Wave 1: sine brightness wave
 *   6  - Wave 2: sawtooth scanning dot
 *   7  - Wave 3: individual oscillation
 *   8  - Wave 4: full ring pulse
 *   9  - Move a dot
 *   10 - Color wipe        (alternates between hue and opposite)
 *   11 - Palette rotate    (gradient spin, speed = direction & rate)
 *   12 - Candle            (speed = flicker rate, fadeout = depth)
 *   13 - Sunrise/sunset    (slow hue build from red to yellow)
 *   14 - Breathing         (whole ring inhales/exhales)
 *   15 - Ocean wave        (rolling brightness wave)
 *   16 - Northern lights   (noise-based hue drift)
 *   17 - TV ambient        (random color section shifts)  ← default
 */

#include <ESP8266WiFi.h>
#include <espnow.h>
#include "FastLED.h"
#include <EEPROM.h>

#define EEPROM_MAGIC      0xAB
#define EEPROM_ADDR_MAGIC 0
#define EEPROM_ADDR_DATA  1

#define DEVICE_NAME        "Circle"
#define MAX_MODE           17

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

// Half circle rotation state
float halfCircleOffset = 0.0f;

// Color wipe state
int   wipePos       = 0;
bool  wipeReverse   = false;
unsigned long previousMillisWipe = 0;

// Palette rotate state
float paletteOffset = 0.0f;

// Candle state
uint16_t candleZ = 0;

// Sunrise state
float sunriseProgress = 0.0f;

// Breathing state
float breathPhase = 0.0f;

// Ocean wave state
float wavePhase = 0.0f;

// Northern lights state
uint16_t northernZ = 0;
float    northernHueShift = 0.0f;

// TV ambient state
uint8_t  tvHue[NUM_LEDS];
uint8_t  tvBri[NUM_LEDS];
unsigned long previousMillisTv = 0;

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
  int  maxMode;        // Max mode index, sent by lamp during discovery
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
    response.maxMode = MAX_MODE;
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

// ---------- SOLID COLOR ----------
void solidColor() {
  fill_solid(leds, NUM_LEDS, CHSV(hue, sat, val));
}

// ---------- CONFETTI ----------
void confetti() {
  if (fadeout > 0) {
    uint8_t fadeAmount = map(fadeout, 0, 255, 2, 30);
    fadeToBlackBy(leds, NUM_LEDS, fadeAmount);
  }

  unsigned long now           = millis();
  unsigned long spawnInterval = map(speed, 0, 255, 600, 16);

  if (now - previousMillisConfetti >= spawnInterval) {
    previousMillisConfetti = now;
    int pos = random16(NUM_LEDS);
    leds[pos] += CHSV(hue, sat, val);
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
  return map(speed, 0, 255, 3, 60);
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
// ---------- HALF CIRCLE ----------
void halfCircle() {
  float rotationSpeed = ((speed - 128) / 128.0f) * 0.25f;
  halfCircleOffset += rotationSpeed;
  if (halfCircleOffset >= NUM_LEDS) halfCircleOffset -= NUM_LEDS;
  if (halfCircleOffset < 0) halfCircleOffset += NUM_LEDS;

  uint8_t oppositeHue = hue + 128;
  for (int i = 0; i < NUM_LEDS; i++) {
    int pos = (int)(i + halfCircleOffset) % NUM_LEDS;
    leds[i] = (pos < NUM_LEDS / 2) ? CHSV(hue, sat, val) : CHSV(oppositeHue, sat, val);
  }
}

// ---------- COLOR WIPE ----------
void colorWipe() {
  unsigned long wipeInterval = 1 + (unsigned long)((1.0f - speed / 255.0f) * 80.0f);
  unsigned long now = millis();
  if (now - previousMillisWipe >= wipeInterval) {
    previousMillisWipe = now;
    uint8_t oppositeHue = hue + 128;
    leds[wipePos] = wipeReverse ? CHSV(oppositeHue, sat, val) : CHSV(hue, sat, val);
    wipePos++;
    if (wipePos >= NUM_LEDS) {
      wipePos = 0;
      wipeReverse = !wipeReverse;
    }
  }
}

// ---------- PALETTE ROTATE ----------
void paletteRotate() {
  float rotationSpeed = ((speed - 128) / 128.0f) * 0.4f;
  paletteOffset += rotationSpeed;
  if (paletteOffset >= NUM_LEDS) paletteOffset -= NUM_LEDS;
  if (paletteOffset < 0) paletteOffset += NUM_LEDS;

  for (int i = 0; i < NUM_LEDS; i++) {
    int pos = (int)(i + paletteOffset) % NUM_LEDS;
    uint8_t blendedHue = hue + (uint8_t)((pos / (float)NUM_LEDS) * 128);
    leds[i] = CHSV(blendedHue, sat, val);
  }
}

// ---------- CANDLE FLAME ----------
void candle() {
  uint8_t noiseSpeed = map(speed, 0, 255, 3, 15);
  candleZ += noiseSpeed;
  uint8_t minBri = val - map(fadeout, 0, 255, 0, val);
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t n = inoise8(i * 15, candleZ);
    uint8_t brightness = map(n, 0, 255, minBri, val);
    leds[i] = CHSV(hue, sat, brightness);
  }
}

// ---------- SUNRISE / SUNSET ----------
void sunrise() {
  float step = 0.00005f + (speed / 255.0f) * 0.0005f;
  sunriseProgress += step;
  if (sunriseProgress > 1.0f) sunriseProgress = 0.0f;
  uint8_t h = (uint8_t)(sunriseProgress * 30);
  uint8_t v = (uint8_t)(sunriseProgress * val);
  fill_solid(leds, NUM_LEDS, CHSV(h, sat, v));
}

// ---------- BREATHING ----------
void breathing() {
  float breathSpeed = 0.005f + (speed / 255.0f) * 0.03f;
  breathPhase += breathSpeed;
  uint8_t brightness = (uint8_t)((sin(breathPhase) * 0.5f + 0.5f) * val);
  fill_solid(leds, NUM_LEDS, CHSV(hue, sat, brightness));
}

// ---------- OCEAN WAVE ----------
void oceanWave() {
  float waveSpeed = 0.01f + (speed / 255.0f) * 0.1f;
  wavePhase += waveSpeed;
  for (int i = 0; i < NUM_LEDS; i++) {
    float angle = (i / (float)NUM_LEDS) * TWO_PI * 2;
    uint8_t brightness = (uint8_t)((sin(angle + wavePhase) * 0.5f + 0.5f) * val);
    leds[i] = CHSV(hue, sat, brightness);
  }
}

// ---------- NORTHERN LIGHTS ----------
void northernLights() {
  uint8_t noiseSpeed = map(speed, 0, 255, 1, 4);
  northernZ += noiseSpeed;
  northernHueShift += 0.1f;
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t n = inoise8(i * 20, northernZ);
    uint8_t h = hue + (uint8_t)(sin(i * 0.3f + northernHueShift) * 20);
    uint8_t v = map(n, 0, 255, 0, val);
    leds[i] = CHSV(h, sat, v);
  }
}

// ---------- TV AMBIENT ----------
void tvAmbient() {
  unsigned long interval = map(speed, 0, 255, 800, 80);
  unsigned long now = millis();
  if (previousMillisTv == 0) {
    for (int i = 0; i < NUM_LEDS; i++) { tvHue[i] = hue; tvBri[i] = val; }
  }
  if (now - previousMillisTv >= interval) {
    previousMillisTv = now;
    int start = random8(NUM_LEDS);
    int len   = random8(4, 12);
    uint8_t newHue = hue + random8(60) - 30;
    uint8_t newBri = val - random8(60);
    for (int i = 0; i < len; i++) {
      int idx = (start + i) % NUM_LEDS;
      tvHue[idx] = newHue;
      tvBri[idx] = newBri;
    }
  }
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CHSV(tvHue[i], sat, tvBri[i]);
  }
}

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
    case 4:
      halfCircle();
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
      moveADot();
      break;
    case 10:
      colorWipe();
      break;
    case 11:
      paletteRotate();
      break;
    case 12:
      candle();
      break;
    case 13:
      sunrise();
      break;
    case 14:
      breathing();
      break;
    case 15:
      oceanWave();
      break;
    case 16:
      northernLights();
      break;
    case 17:
    default:
      tvAmbient();
      break;
  }
}
