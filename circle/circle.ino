/*
 * Circle / NanoCircle - ESP-NOW LED Ring Fixture
 * Jonas Vorwerk 2026
 *
 * Uncomment ONE device below before flashing:
 *
 *   DEVICE_NANOCIRCLE  →  ESP8266 + WS2812  (60 LEDs)   Data: GPIO 12
 *   DEVICE_CIRCLE      →  ESP8266 + APA102  (300 LEDs)  Data: GPIO 14 | Clock: GPIO 12
 *
 * Modes (set via remote):
 *   0  - Move a dot        ← default
 *   1  - Solid color
 *   2  - Confetti          (speed = spawn rate, fadeout = fade depth)
 *   3  - Noise flow        (Perlin noise, fixed hue, fadeout = blob size)
 *   4  - Noise flow hue    (Perlin noise, drifting hue, fadeout = blob size)
 *   5  - Half circle       (rotating split, speed = direction & rate, fadeout = blend edge)
 *   6  - Wave              (sine brightness wave)
 *   7  - Color wipe        (alternates between hue and opposite, fadeout = blend width)
 *   8  - Palette rotate    (gradient spin, speed = direction & rate, fadeout = gradient width)
 *   9  - Candle            (speed = flicker rate, fadeout = flicker depth)
 *   10 - Breathing         (whole ring inhales/exhales)
 *   11 - Northern lights   (noise-based hue drift, fadeout = dark patch depth)
 */

// ============================================================
// DEVICE SELECTION — uncomment ONE:
// ============================================================
#define DEVICE_NANOCIRCLE
// #define DEVICE_CIRCLE
// #define DEVICE_CIRCLE_180
// ============================================================

// Uncomment to wipe EEPROM and boot with defaults (re-comment and reflash after!)
// #define EEPROM_RESET

#include <ESP8266WiFi.h>
#include <espnow.h>
#include "FastLED.h"
#include <EEPROM.h>

#define EEPROM_MAGIC      0xAB
#define EEPROM_ADDR_MAGIC 0
#define EEPROM_ADDR_DATA  1

// ---------- DEVICE CONFIG ----------
#ifdef DEVICE_NANOCIRCLE
  #define DEVICE_NAME       "NanoCircle"
  #define NUM_LEDS          60
#elif defined(DEVICE_CIRCLE)
  #define DEVICE_NAME       "Circle"
  #define NUM_LEDS          300
  #define DATA_PIN          14
  #define CLOCK_PIN         12
#elif defined(DEVICE_CIRCLE_180)
  #define DEVICE_NAME       "Circle 180"
  #define NUM_LEDS          180
  #define DATA_PIN          14
  #define CLOCK_PIN         12
#else
  #error "No device selected — uncomment DEVICE_NANOCIRCLE, DEVICE_CIRCLE or DEVICE_CIRCLE_180 above"
#endif

#define BRIGHTNESS         200
#define MAX_MODE           15
#define FRAMES_PER_SECOND  60

CRGB leds[NUM_LEDS];

// ---------- CURRENT STATE ----------
uint8_t hue     = 30;
uint8_t sat     = 200;
uint8_t val     = 200;
bool    power   = true;
int     mode    = 0;
uint8_t speed   = 128;
uint8_t fadeout = 10;

// ---------- MODE NAMES SEND STATE ----------
bool    pendingSendModeNames = false;
uint8_t pendingModeNamesMac[6];
int     pendingModeNamesIdx  = 0;
unsigned long previousMillisModeNames = 0;

// ---------- ANIMATION STATE ----------
uint16_t noiseZ              = 0;
uint16_t noiseScale          = 20;
uint8_t  driftHue            = 0;
unsigned long previousMillisHue      = 0;
unsigned long previousMillisConfetti = 0;
float    halfCircleOffset    = 0.0f;
int      wipePos             = 0;
bool     wipeReverse         = false;
unsigned long previousMillisWipe = 0;
float    paletteOffset       = 0.0f;
uint16_t candleZ             = 0;
float    breathPhase         = 0.0f;
uint16_t northernZ           = 0;
float    northernHueShift    = 0.0f;
float    burstPos            = 0.0f;
uint8_t  sparkleBri[NUM_LEDS];
#define  NUM_BLOBS 4
float    blobPos[NUM_BLOBS];
float    blobSpd[NUM_BLOBS];
int8_t   blobHueOff[NUM_BLOBS];
bool     blobsInitialized    = false;
#define  NUM_CLOUDS 4
float    cloudPos[NUM_CLOUDS];
float    cloudSpd[NUM_CLOUDS];
uint8_t  cloudHue[NUM_CLOUDS];
bool     cloudsInitialized   = false;

// ---------- MOVE A DOT STATE ----------
#define NUM_DOTS              5
#define MAX_DOT_AGE           500
#define SPEED_DOTS_DIFFERENCE 50
#define HUE_SPREAD            8
#define SAT_SPREAD            200
#define VAL_SPREAD            40

float   pos_dot[NUM_DOTS];
float   spd_dot[NUM_DOTS];
int     age_dot[NUM_DOTS];
int8_t  hue_offset[NUM_DOTS];
uint8_t sat_reduction[NUM_DOTS];
uint8_t val_reduction[NUM_DOTS];
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

#ifdef DEVICE_NANOCIRCLE
static const LampPreset lampPresets[MAX_PRESETS] = {
  { "Circle",   30,  200, 220, true,  0, 30,  59 },
  { "Dimmed",   30,  200,  73, true,  0, 49,   0 },
  { "Sun",      20,  220, 200, true,  0, 128, 10 },
  { "Ocean",   165,  255, 177, true,  0, 67,  81 },
  { "Confetti", 80,  255, 255, true,  2, 180, 80 },
  { "Warm",     18,  200, 255, true,  1, 128, 10 },
  { "Wave",     30,  200, 255, true,  6, 42,   0 },
  { "Glow",     48,  180, 160, true,  3, 42,  10 },
};
#elif defined(DEVICE_CIRCLE) || defined(DEVICE_CIRCLE_180)
static const LampPreset lampPresets[MAX_PRESETS] = {
  { "Warm",     18,  200, 255, true,  1, 128, 10 },
  { "Cool",    155,  130, 230, true,  1, 128, 10 },
  { "Vivid",     0,  255, 255, true,  1, 128, 10 },
  { "Ocean",   135,  255, 200, true,  1, 128, 10 },
  { "Confetti", 80,  255, 255, true,  2, 180, 80 },
  { "Dots",     30,  200, 220, true,  0, 128, 10 },
  { "Wave",    170,  255, 220, true,  6, 150, 10 },
  { "Night",    20,  220,  60, true,  1, 128, 10 },
};
#endif

// ---------- MODE NAMES PACKET ----------
// One packet per mode — no size matching needed, remote works with any number of modes
#define MAX_MODE_NAME_LEN 16
typedef struct {
  bool    isModeNamePacket;  // single name
  uint8_t modeIndex;
  uint8_t totalModes;        // total modes the fixture has
  char    name[MAX_MODE_NAME_LEN];
} mode_name_packet;          // 19 bytes total

static const char* modeNames[] = {
  "Move a Dot",
  "Solid Color",
  "Confetti",
  "Noise Flow",
  "Noise Hue",
  "Half Circle",
  "Wave",
  "Color Wipe",
  "Palette",
  "Candle",
  "Breathing",
  "Northern Lights",
  "Sparkle Burst",
  "Lava Lamp",
  "Clouds",
  "Segments",
};

// ---------- ESP-NOW MESSAGE STRUCT ----------
typedef struct struct_message {
  uint8_t h;
  uint8_t s;
  uint8_t v;
  bool    power;
  int     mode;
  uint8_t speed;
  uint8_t fadeout;
  bool    requestState;     // If true: reply with current state, don't apply changes
  bool    isDiscovery;      // If true: discovery broadcast — reply with device name
  char    deviceName[16];   // Device name sent in discovery reply
  int     maxMode;          // Max mode index, sent by lamp during discovery
  bool    requestPresets;   // If true: reply with preset list
  bool    requestModeNames; // If true: reply with mode names list
} struct_message;

struct_message incomingData;

// ---------- ESP-NOW CALLBACK ----------
void onDataRecv(uint8_t *mac, uint8_t *incomingDataBytes, uint8_t len) {
  memcpy(&incomingData, incomingDataBytes, sizeof(incomingData));

  if (incomingData.isDiscovery) {
    struct_message response = {};
    response.isDiscovery = true;
    strncpy(response.deviceName, DEVICE_NAME, sizeof(response.deviceName) - 1);
    response.maxMode = MAX_MODE;
    esp_now_add_peer(mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
    esp_now_send(mac, (uint8_t*)&response, sizeof(response));
    Serial.println("Discovery — sending name to remote");
    return;
  }

  if (incomingData.requestState) {
    struct_message response = {};
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
    Serial.println("State request — sending state to remote");
    return;
  }

  if (incomingData.requestPresets) {
    preset_packet pp = {};
    pp.isPresetPacket = true;
    pp.count = MAX_PRESETS;
    for (int i = 0; i < MAX_PRESETS; i++) pp.slots[i] = lampPresets[i];
    esp_now_add_peer(mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
    esp_now_send(mac, (uint8_t*)&pp, sizeof(pp));
    Serial.println("Preset request — sending presets to remote");
    return;
  }

  if (incomingData.requestModeNames) {
    esp_now_add_peer(mac, ESP_NOW_ROLE_COMBO, 1, NULL, 0);
    memcpy(pendingModeNamesMac, mac, 6);
    pendingModeNamesIdx  = 0;
    pendingSendModeNames = true;
    return;
  }

  hue     = incomingData.h;
  sat     = incomingData.s;
  val     = incomingData.v;
  power   = incomingData.power;
  mode    = incomingData.mode;
  speed   = incomingData.speed;
  fadeout = incomingData.fadeout;

  EEPROM.write(EEPROM_ADDR_MAGIC, EEPROM_MAGIC);
  EEPROM.put(EEPROM_ADDR_DATA, incomingData);
  EEPROM.commit();

  Serial.printf("ESP-NOW -> H:%d S:%d V:%d Power:%d Mode:%d Speed:%d Fade:%d\n",
                hue, sat, val, power, mode, speed, fadeout);
}

// ---------- SETUP ----------
void setup() {
  Serial.begin(115200);
  Serial.printf("%s booting...\n", DEVICE_NAME);

  EEPROM.begin(1 + sizeof(struct_message));

#ifdef EEPROM_RESET
  EEPROM.write(EEPROM_ADDR_MAGIC, 0x00);
  EEPROM.commit();
  Serial.println("EEPROM reset — using defaults. Re-comment EEPROM_RESET and reflash!");
#else
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
    Serial.println("No saved settings — using defaults.");
  }
#endif

  WiFi.mode(WIFI_STA);
  Serial.print("MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != 0) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_recv_cb(onDataRecv);
  Serial.println("ESP-NOW ready");

#ifdef DEVICE_NANOCIRCLE
  FastLED.addLeds<WS2812, 12, GRB>(leds, NUM_LEDS).setCorrection(TypicalSMD5050);
#elif defined(DEVICE_CIRCLE) || defined(DEVICE_CIRCLE_180)
  FastLED.addLeds<APA102, DATA_PIN, CLOCK_PIN, BGR>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
#endif

  FastLED.setBrightness(BRIGHTNESS);
  fill_solid(leds, NUM_LEDS, CHSV(hue, sat, val));
  FastLED.show();

  noiseZ = random16();
}

// ---------- SERIAL CONTROL ----------
void checkSerial() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();

  if (cmd == "GET") {
    Serial.printf("STATE:%d,%d,%d,%d,%d,%d,%d\n", hue, sat, val, power, mode, speed, fadeout);
    return;
  }
  if (cmd == "NAME") {
    Serial.println("NAME:" DEVICE_NAME);
    return;
  }

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
  // Send mode names one at a time — non-blocking, 20ms between packets
  if (pendingSendModeNames) {
    unsigned long now = millis();
    if (now - previousMillisModeNames >= 20) {
      previousMillisModeNames = now;
      mode_name_packet mn = {};
      mn.isModeNamePacket = true;
      mn.modeIndex        = pendingModeNamesIdx;
      mn.totalModes       = MAX_MODE + 1;
      strncpy(mn.name, modeNames[pendingModeNamesIdx], MAX_MODE_NAME_LEN - 1);
      esp_now_send(pendingModeNamesMac, (uint8_t*)&mn, sizeof(mn));
      pendingModeNamesIdx++;
      if (pendingModeNamesIdx > MAX_MODE) {
        pendingSendModeNames = false;
        Serial.printf("Mode names sent (%d modes)\n", MAX_MODE + 1);
      }
    }
  }

  checkSerial();
  displayLeds();
  FastLED.show();
  FastLED.delay(1000 / FRAMES_PER_SECOND);
}

// ============================================================
// ANIMATIONS
// ============================================================

// ---------- SOLID COLOR ----------
void solidColor() {
  fill_solid(leds, NUM_LEDS, CHSV(hue, sat, val));
}

// ---------- CONFETTI ----------
// speed = spawn rate | fadeout = fade depth (0 = no fade)
void confetti() {
  if (fadeout > 0) {
    uint8_t fadeAmount = map(fadeout, 0, 255, 2, 30);
    fadeToBlackBy(leds, NUM_LEDS, fadeAmount);
  }
  unsigned long now           = millis();
  unsigned long spawnInterval = map(speed, 0, 255, 600, 16);
  if (now - previousMillisConfetti >= spawnInterval) {
    previousMillisConfetti = now;
    leds[random16(NUM_LEDS)] += CHSV(hue, sat, val);
  }
}

// ---------- NOISE FLOW ----------
// speed = flow speed | fadeout = blob size (low = fine grain, high = wide blobs)
void noiseFlow() {
  uint8_t  noiseSpeed    = map(speed, 0, 255, 1, 8);
  uint16_t dynamicScale  = map(fadeout, 0, 255, 5, 80);
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t n          = inoise8(i * dynamicScale, noiseZ);
    uint8_t brightness = map(n, 0, 255, 0, val);
    leds[i] = CHSV(hue, sat, brightness);
  }
  noiseZ += noiseSpeed;
}

// ---------- NOISE FLOW HUE ----------
// speed = flow speed | fadeout = blob size
void noiseFlowHue() {
  uint8_t  noiseSpeed   = map(speed, 0, 255, 1, 8);
  uint16_t dynamicScale = map(fadeout, 0, 255, 5, 80);
  unsigned long now = millis();
  if (now - previousMillisHue >= 500) {
    previousMillisHue = now;
    driftHue++;
  }
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t n          = inoise8(i * dynamicScale, noiseZ);
    uint8_t brightness = map(n, 0, 255, 0, val);
    leds[i] = CHSV(driftHue, sat, brightness);
  }
  noiseZ += noiseSpeed;
}

// ---------- HALF CIRCLE ----------
// speed = rotation direction & rate (128 = stopped) | fadeout = blend edge (0 = wide, 255 = hard)
void halfCircle() {
  float rotationSpeed = ((speed - 128) / 128.0f) * 0.25f;
  halfCircleOffset += rotationSpeed;
  if (halfCircleOffset >= NUM_LEDS) halfCircleOffset -= NUM_LEDS;
  if (halfCircleOffset < 0)         halfCircleOffset += NUM_LEDS;

  int half = NUM_LEDS / 2;

  for (int i = 0; i < NUM_LEDS; i++) {
    int  pos      = (int)(i + halfCircleOffset + NUM_LEDS) % NUM_LEDS;
    bool inHalfA  = (pos < half);

    // Distance to nearest boundary, normalised: 0.0 = at boundary, 1.0 = center of half
    float dist     = inHalfA ? min(pos, half - pos)
                              : min(pos - half, NUM_LEDS - pos);
    float normDist = constrain(dist / (half / 2.0f), 0.0f, 1.0f);

    // How much to blend toward the opposite color — capped at 0.5 so halves stay distinct
    float blendAmt = (1.0f - normDist) * (fadeout / 255.0f) * 0.5f;

    // Interpolate hue in HSV space — no muddy RGB mixing
    uint8_t blendedHue = inHalfA ? (uint8_t)(hue + blendAmt * 128)
                                  : (uint8_t)(hue + 128 - blendAmt * 128);
    leds[i] = CHSV(blendedHue, sat, val);
  }
}

// ---------- WAVE ----------
// speed = BPM | fadeout = wave width (0 = narrow bright peak, 255 = wide nearly full ring)
void wave() {
  uint8_t bpm    = map(speed, 0, 255, 3, 20);
  uint8_t minBri = map(fadeout, 0, 255, 0, val);
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t phase      = (uint8_t)map(i, 0, NUM_LEDS - 1, 0, 255);
    uint8_t brightness = beatsin8(bpm, minBri, val, 0, phase);
    leds[i] = CHSV(hue, sat, brightness);
  }
}

// ---------- COLOR WIPE ----------
// speed = wipe speed | fadeout = blend width at the transition front
void colorWipe() {
  unsigned long wipeInterval = 1 + (unsigned long)((1.0f - speed / 255.0f) * 80.0f);
  unsigned long now = millis();
  if (now - previousMillisWipe >= wipeInterval) {
    previousMillisWipe = now;
    wipePos++;
    if (wipePos >= NUM_LEDS) {
      wipePos = 0;
      wipeReverse = !wipeReverse;
    }
  }

  uint8_t newHue     = wipeReverse ? (uint8_t)(hue + 128) : hue;
  uint8_t prevHue    = wipeReverse ? hue : (uint8_t)(hue + 128);
  int     blendWidth = max(2, (int)map(fadeout, 0, 255, 2, NUM_LEDS / 2));

  // Pre-compute once — avoids expensive HSV→RGB conversion on every LED every frame
  CRGB colNew  = CHSV(newHue,  sat, val);
  CRGB colPrev = CHSV(prevHue, sat, val);

  for (int i = 0; i < NUM_LEDS; i++) {
    int dist = (wipePos - i + NUM_LEDS) % NUM_LEDS;
    if (dist < blendWidth) {
      uint8_t t = map(dist, 0, blendWidth - 1, 255, 0);
      leds[i] = blend(colNew, colPrev, t);
    } else {
      leds[i] = colNew;
    }
  }
}

// ---------- PALETTE ROTATE ----------
// speed = rotation direction & rate (128 = stopped) | fadeout = gradient spread width
void paletteRotate() {
  float rotationSpeed = ((speed - 128) / 128.0f) * 0.4f;
  paletteOffset += rotationSpeed;
  if (paletteOffset >= NUM_LEDS) paletteOffset -= NUM_LEDS;
  if (paletteOffset < 0)         paletteOffset += NUM_LEDS;

  uint8_t spread = map(fadeout, 0, 255, 4, 128);

  for (int i = 0; i < NUM_LEDS; i++) {
    int     pos        = (int)(i + paletteOffset) % NUM_LEDS;
    uint8_t blendedHue = hue + (uint8_t)((pos / (float)NUM_LEDS) * spread);
    leds[i] = CHSV(blendedHue, sat, val);
  }
}

// ---------- CANDLE ----------
// speed = flicker rate | fadeout = flicker depth (0 = subtle, 255 = dramatic)
void candle() {
  uint8_t noiseSpeed = map(speed, 0, 255, 3, 15);
  candleZ += noiseSpeed;
  uint8_t minBri = val - map(fadeout, 0, 255, 0, val);
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t n          = inoise8(i * 15, candleZ);
    uint8_t brightness = map(n, 0, 255, minBri, val);
    leds[i] = CHSV(hue, sat, brightness);
  }
}

// ---------- BREATHING ----------
// speed = breath rate
void breathing() {
  float breathSpeed = 0.005f + (speed / 255.0f) * 0.03f;
  breathPhase += breathSpeed;
  uint8_t brightness = 100 + (uint8_t)((sin(breathPhase) * 0.5f + 0.5f) * (val - 100));
  fill_solid(leds, NUM_LEDS, CHSV(hue, sat, brightness));
}

// ---------- NORTHERN LIGHTS ----------
// speed = drift speed | fadeout = dark patch depth (0 = no dip, 255 = full black)
void northernLights() {
  uint8_t noiseSpeed = map(speed, 0, 255, 1, 20);
  northernZ += noiseSpeed;
  northernHueShift += 0.1f;
  uint8_t minBri = val - map(fadeout, 0, 255, 0, val);
  for (int i = 0; i < NUM_LEDS; i++) {
    uint8_t n = inoise8(i * 8, northernZ);
    uint8_t h = hue + (uint8_t)(sin(i * 0.15f + northernHueShift) * 20);
    uint8_t v = map(n, 0, 255, minBri, val);
    leds[i] = CHSV(h, sat, v);
  }
}

// ---------- MOVE A DOT ----------
// speed = dot speed | fadeout = trail length
void initDot(int i) {
  int globalSpeed = map(speed, 0, 255, 1, 60);
  int spread      = map(speed, 0, 255, 3, SPEED_DOTS_DIFFERENCE);
  int maxSpd      = constrain(globalSpeed + spread, 0, 100);
  spd_dot[i]       = (float)map(random(100), 0, 100, globalSpeed, maxSpd) / 100.0f
                     * (random(2) * 2 - 1);
  pos_dot[i]       = random(NUM_LEDS);
  age_dot[i]       = random(MAX_DOT_AGE / 10);
  hue_offset[i]    = (int8_t)(random(HUE_SPREAD * 2 + 1) - HUE_SPREAD);
  sat_reduction[i] = random8(SAT_SPREAD + 1);
  val_reduction[i] = random8(VAL_SPREAD + 1);
}

void initAllDots() {
  for (int i = 0; i < NUM_DOTS; i++) initDot(i);
  dotsInitialized = true;
}

void moveADot() {
  if (!dotsInitialized) initAllDots();
  EVERY_N_MILLISECONDS(50) {
    if (fadeout > 0) fadeToBlackBy(leds, NUM_LEDS, map(fadeout, 1, 255, 2, 50));
  }
  for (int i = 0; i < NUM_DOTS; i++) {
    uint8_t dotSat = (uint8_t)max(0, (int)sat - (int)sat_reduction[i]);
    uint8_t dotVal = (uint8_t)max(0, (int)val - (int)val_reduction[i]);
    leds[(int)pos_dot[i]] = CHSV((uint8_t)(hue + hue_offset[i]), dotSat, dotVal);
    pos_dot[i] += spd_dot[i];
    age_dot[i]++;
    if (age_dot[i] > MAX_DOT_AGE) initDot(i);
    if (pos_dot[i] >= NUM_LEDS && spd_dot[i] > 0) pos_dot[i] = 0;
    if (pos_dot[i] < 0         && spd_dot[i] < 0) pos_dot[i] = NUM_LEDS - 1;
  }
}

// ---------- LAVA LAMP ----------
// speed = blob movement speed | fadeout = blob size
void lavaLamp() {
  if (!blobsInitialized) {
    for (int i = 0; i < NUM_BLOBS; i++) {
      blobPos[i]    = random(NUM_LEDS);
      blobSpd[i]    = (random(50) / 100.0f + 0.05f) * (random(2) ? 1 : -1);
      blobHueOff[i] = (int8_t)(random(40) - 20);
    }
    blobsInitialized = true;
  }

  float moveScale = speed / 255.0f;
  for (int i = 0; i < NUM_BLOBS; i++) {
    // Slight random wobble to speed
    blobSpd[i] += (random(100) / 100.0f - 0.5f) * 0.005f;
    blobSpd[i]  = constrain(blobSpd[i], -0.4f, 0.4f);
    blobPos[i] += blobSpd[i] * moveScale;
    if (blobPos[i] >= NUM_LEDS) blobPos[i] -= NUM_LEDS;
    if (blobPos[i] < 0)         blobPos[i] += NUM_LEDS;
  }

  int blobWidth = map(fadeout, 0, 255, 8, NUM_LEDS / 2);

  for (int i = 0; i < NUM_LEDS; i++) {
    float totalWeight = 0;
    float weightedHue = 0;

    for (int b = 0; b < NUM_BLOBS; b++) {
      float dist = fabsf(i - blobPos[b]);
      if (dist > NUM_LEDS / 2) dist = NUM_LEDS - dist;  // ring wrap
      if (dist < blobWidth) {
        float t         = 1.0f - (dist / blobWidth);
        float influence = t * t;
        totalWeight    += influence;
        weightedHue    += influence * (hue + blobHueOff[b]);
      }
    }

    if (totalWeight > 0) {
      uint8_t finalHue = (uint8_t)(weightedHue / totalWeight);
      uint8_t finalVal = (uint8_t)constrain(totalWeight * val, 0, val);
      leds[i] = CHSV(finalHue, sat, finalVal);
    } else {
      leds[i] = CHSV(hue, sat, 0);
    }
  }
}

// ---------- CLOUDS ----------
// speed = movement speed | fadeout = cloud length (short to long, relative to NUM_LEDS)
void clouds() {
  if (!cloudsInitialized) {
    for (int i = 0; i < NUM_CLOUDS; i++) {
      cloudPos[i] = (float)i * (NUM_LEDS / NUM_CLOUDS);
      cloudSpd[i] = (random(30) / 100.0f + 0.05f) * (random(2) ? 1 : -1);
      cloudHue[i] = hue + (i * 64);  // Evenly spaced hues
    }
    cloudsInitialized = true;
  }

  float moveScale  = speed / 255.0f;
  int   cloudLen   = map(fadeout, 0, 255, NUM_LEDS / 8, NUM_LEDS / 2);
  int   edgeLen    = max(2, cloudLen / 4);  // Soft edge width = quarter of cloud

  // Clear LEDs
  fill_solid(leds, NUM_LEDS, CRGB::Black);

  for (int c = 0; c < NUM_CLOUDS; c++) {
    cloudPos[c] += cloudSpd[c] * moveScale;
    if (cloudPos[c] >= NUM_LEDS) cloudPos[c] -= NUM_LEDS;
    if (cloudPos[c] < 0)         cloudPos[c] += NUM_LEDS;

    int center = (int)cloudPos[c];

    for (int j = -cloudLen / 2; j <= cloudLen / 2; j++) {
      int idx = (center + j + NUM_LEDS) % NUM_LEDS;
      int distFromEdge = abs(j);
      int distToEdge   = cloudLen / 2 - distFromEdge;

      // Soft fade at edges
      uint8_t brightness;
      if (distToEdge >= edgeLen) {
        brightness = val;  // Flat top
      } else {
        brightness = map(distToEdge, 0, edgeLen, 0, val);
      }

      // Add to existing LED (blend overlapping clouds)
      CRGB existing = leds[idx];
      CRGB newCol   = CRGB(CHSV(cloudHue[c], sat, brightness));
      leds[idx] = existing + newCol;
    }
  }
}

// ---------- SEGMENTS ----------
// speed = movement speed | fadeout = segment length (relative to NUM_LEDS)
void segments() {
  if (!cloudsInitialized) {
    for (int i = 0; i < NUM_CLOUDS; i++) {
      cloudPos[i] = (float)i * (NUM_LEDS / NUM_CLOUDS);
      cloudSpd[i] = (random(30) / 100.0f + 0.05f) * (random(2) ? 1 : -1);
      cloudHue[i] = hue + (i * 64);
    }
    cloudsInitialized = true;
  }

  float moveScale = speed / 255.0f;
  int   segLen    = map(fadeout, 0, 255, NUM_LEDS / 8, NUM_LEDS / 2);

  fill_solid(leds, NUM_LEDS, CRGB::Black);

  for (int c = 0; c < NUM_CLOUDS; c++) {
    cloudPos[c] += cloudSpd[c] * moveScale;
    if (cloudPos[c] >= NUM_LEDS) cloudPos[c] -= NUM_LEDS;
    if (cloudPos[c] < 0)         cloudPos[c] += NUM_LEDS;

    int start = (int)(cloudPos[c] - segLen / 2 + NUM_LEDS) % NUM_LEDS;
    CRGB col  = CHSV(cloudHue[c], sat, val);

    for (int j = 0; j < segLen; j++) {
      int idx    = (start + j) % NUM_LEDS;
      leds[idx] += col;  // Add — overlapping segments blend colors
    }
  }
}

// ---------- SPARKLE BURST ----------
// speed = focal point movement | fadeout = cluster width (tight vs spread)
void sparkleBurst() {
  // Move focal point around the ring
  float moveSpeed = (speed / 255.0f) * 0.5f;
  burstPos += moveSpeed;
  if (burstPos >= NUM_LEDS) burstPos -= NUM_LEDS;

  // Fade all sparkles
  for (int i = 0; i < NUM_LEDS; i++) {
    if (sparkleBri[i] > 3) sparkleBri[i] -= 3; else sparkleBri[i] = 0;
  }

  // Cluster width: fadeout=0 → half the ring, fadeout=255 → very tight
  int clusterWidth = map(fadeout, 0, 255, 4, NUM_LEDS);

  // Spawn a few sparkles near the focal point each frame
  int spawns = random(1, 4);
  for (int s = 0; s < spawns; s++) {
    int offset = random(-clusterWidth / 2, clusterWidth / 2);
    int idx    = ((int)burstPos + offset + NUM_LEDS) % NUM_LEDS;
    sparkleBri[idx] = val;
  }

  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CHSV(hue, sat, sparkleBri[i]);
  }
}

// ---------- DISPLAY ----------
void displayLeds() {
  if (!power) { FastLED.clear(); return; }
  switch (mode) {
    case 0:  moveADot();       break;
    case 1:  solidColor();     break;
    case 2:  confetti();       break;
    case 3:  noiseFlow();      break;
    case 4:  noiseFlowHue();   break;
    case 5:  halfCircle();     break;
    case 6:  wave();           break;
    case 7:  colorWipe();      break;
    case 8:  paletteRotate();  break;
    case 9:  candle();         break;
    case 10: breathing();      break;
    case 11: northernLights();  break;
    case 12: sparkleBurst();    break;
    case 13: lavaLamp();         break;
    case 14: clouds();           break;
    case 15:
    default: segments();         break;
  }
}
