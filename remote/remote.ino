/*
 * RGB LED Remote Control - M5Stack CoreS3/SE
 * 
 * Features:
 * - 2D HSV color wheel (like traditional color picker)
 * - Hue: angle around the circle
 * - Brightness/Lightness: radial distance (center=white/bright, edge=dark/vivid)
 * - Saturation: radial distance (center=desaturated/white, edge=saturated)
 * - Display timeout after 10 seconds with pickup wake
 * - Sleep mode after 60 seconds with accelerometer wake
 * - ESP-NOW transmission to Light device
 */

#include <M5Unified.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_sleep.h>
// Maximum number of discoverable lights (runtime count stored in numLights)
#define MAX_LIGHTS 8

// Known device name → maxMode mapping.
// Used to set the correct mode ceiling when a light announces itself during discovery.
static const struct { const char* name; int maxMode; } knownDevices[] = {
  { "Circle",   9 },
  { "Painting", 8 },
};
static const int NUM_KNOWN_DEVICES = (int)(sizeof(knownDevices) / sizeof(knownDevices[0]));

// Broadcast MAC for ESP-NOW discovery
static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

uint8_t lightMacAddresses[MAX_LIGHTS][6];  // Filled at runtime during discovery

// Display settings
#define DISPLAY_TIMEOUT 30000   // 30 seconds
#define SLEEP_TIMEOUT 120000    // 120 seconds (2 minutes)

// Light color bar at top
#define LIGHT_BAR_HEIGHT 25         // Increased (was 20)
#define LIGHT_BAR_Y 0
#define LIGHT_BAR_INDICATOR_HEIGHT 3  // Active light indicator thickness
#define LIGHT_BAR_GAP 3  // Gap between light bar and content below

// Page system - new flow
#define NUM_PAGES 6
#define PAGE_LOADING        0
#define PAGE_LIGHT_SELECT   1
#define PAGE_LIGHT_MAIN     2
#define PAGE_PRESETS        3   // Settings group: first sub-page
#define PAGE_COLOR_SELECTOR 4
#define PAGE_SETTINGS       5
#define IS_SETTINGS_PAGE(p) ((p) >= PAGE_PRESETS && (p) <= PAGE_SETTINGS)

// Auto-return timeout
#define AUTO_RETURN_TIMEOUT 5000  // 3 seconds

// Auto-return toggle button (small triangle, top-left corner of light bar)
#define AUTO_BTN_HIT 25  // hit-area: 0..24 in both X and Y (fits inside the 25px bar)

// Page button (lower right corner)
#define PAGE_BUTTON_X 285
#define PAGE_BUTTON_Y 215
#define PAGE_BUTTON_RADIUS 20

// Brightness slider (Page 2) - full width, no title
#define BRIGHTNESS_SLIDER_X 0       // Full width (was 60)
#define BRIGHTNESS_SLIDER_Y (LIGHT_BAR_HEIGHT + LIGHT_BAR_GAP + 30)  // Below gap + some spacing
#define BRIGHTNESS_SLIDER_WIDTH 320 // Full screen width (was 200)
#define BRIGHTNESS_SLIDER_HEIGHT 60 // Taller (was 30)

// Color wheel settings - rectangular, full width
#define WHEEL_X 0               // Start at left edge
#define WHEEL_Y (LIGHT_BAR_HEIGHT + LIGHT_BAR_GAP)  // Below light bar + gap (28px)
#define WHEEL_WIDTH 320         // Full screen width

// Settings sub-pages bottom nav bar
#define SNAV_H       36
#define SNAV_Y       (240 - SNAV_H)              // 204 px from top
#define SNAV_BTN_W   60
#define SNAV_TITLE_X SNAV_BTN_W
#define SNAV_TITLE_W (320 - SNAV_BTN_W * 2)
#define SNAV_COL_ACT 0x630C    // dark grey (~#606060)

#define WHEEL_HEIGHT (SNAV_Y - WHEEL_Y)          // 204 - 28 = 176 px

// Battery indicator settings (top right corner with padding)
#define BATTERY_INDICATOR_X 313  // 5px padding: 320 - 5 - (radius*2) = 320 - 5 - 2 = 313 (center)
#define BATTERY_INDICATOR_Y 7    // 5px padding: 0 + 5 + radius = 5 + 2 = 7 (center)
#define BATTERY_INDICATOR_RADIUS 2  // 2px radius = 4px diameter (was 3)

// Button dimensions for new UI
#define BUTTON_MARGIN 20
#define BUTTON_HEIGHT 60
#define BUTTON_SPACING 10

// Accelerometer wake threshold
#define ACCEL_THRESHOLD 0.1  // g-force threshold for wake (very sensitive)

// ---------- GLOBAL VARIABLES ----------
// Current page
uint8_t currentPage = PAGE_LIGHT_SELECT;

// Current light selection
uint8_t currentLight = 0; // 0, 1, or 2 for lights 1, 2, 3

// Store HSV values for each light independently
uint8_t lightHue[MAX_LIGHTS];
uint8_t lightSat[MAX_LIGHTS];
uint8_t lightVal[MAX_LIGHTS];
bool lightOn[MAX_LIGHTS];
int  lightMode[MAX_LIGHTS];
uint8_t lightSpeed[MAX_LIGHTS];
uint8_t lightFadeout[MAX_LIGHTS];
int  lightMaxMode[MAX_LIGHTS];    // Highest valid mode number for each fixture
char lightNames[MAX_LIGHTS][16];  // Light names (max 15 chars + null terminator)

// Current HSV values (for active light)
uint8_t currentHue = 30;
uint8_t currentSat = 200;
uint8_t currentVal = 200;
bool currentOn = true;       // Current light ON/OFF state
int currentMode = 0;         // Current light mode (0=solid, 1=confetti)
uint8_t currentSpeed = 128;  // Current animation speed (0-255, default medium)
uint8_t currentFadeout = 10; // Current fadeout rate (0-255, default 10)

// Preset definitions — loaded from each lamp via ESP-NOW
#define MAX_PRESETS 8

struct LampPreset {
  char    name[12];
  uint8_t h, s, v;
  bool    power;
  uint8_t mode;     // uint8_t — max mode value is 9
  uint8_t speed;
  uint8_t fadeout;
};

typedef struct {
  bool       isPresetPacket;
  uint8_t    count;
  LampPreset slots[MAX_PRESETS];
} preset_packet;

// Per-lamp preset storage (filled on demand from each lamp)
LampPreset lightPresets[MAX_LIGHTS][MAX_PRESETS];
uint8_t    lightPresetCount[MAX_LIGHTS];
bool       lightPresetsLoaded[MAX_LIGHTS];

// Touch state
bool displayOn = true;
unsigned long lastTouchTime = 0;
unsigned long lastPageChangeTime = 0;  // For auto-return
bool wasTouching = false;
bool touchLocked = false;
bool colorWheelActive = false;
bool sliderWasUsed = false;  // Track if slider was dragged on settings page

// Auto-return toggle
bool autoReturnEnabled = true;  // When false, pages stay open until manually navigated away

// USB/charging state tracking for event detection
bool lastChargingState = false;

// Page navigation history for auto-return
uint8_t previousPage = PAGE_LIGHT_SELECT;
const char* settingsPageNames[] = { "Presets", "Color", "Settings" };
bool ignoreFirstTouch = false;  // Ignore first touch after page navigation

// Single debounce variable and time for ALL buttons, sliders, interactions
#define DEBOUNCE 400  // ms - applies to all buttons
unsigned long lastInteractionTime = 0;

bool ignoreNextTouch = false;
unsigned long ignoreUntilTime = 0;

// Color wheel canvas sprites (pre-rendered background + working canvas)
M5Canvas canvas(&M5.Display);
M5Canvas wheelBg(&M5.Display);
float cwX = 0.5f, cwY = 0.25f;  // Normalised cursor position on color wheel
bool  wheelReady = false;        // True once wheelBg has been rendered

// ESP-NOW message structure (matches Light device)
typedef struct struct_message {
  uint8_t h;
  uint8_t s;
  uint8_t v;
  bool power;
  int mode;
  uint8_t speed;        // Animation speed (0-255)
  uint8_t fadeout;      // Fadeout rate (0-255)
  bool requestState;    // If true: light should reply with its state, not apply changes
  bool isDiscovery;     // If true: discovery broadcast — light should reply with its name
  char deviceName[16];  // Device name (sent in discovery reply)
  bool requestPresets;  // If true: light should reply with its preset list
} struct_message;

struct_message outgoingData;

// Runtime device count (filled during discovery)
int numLights = 0;

// ESP-NOW peer info
esp_now_peer_info_t peerInfo;

// State-read tracking (set by receive callback during requestLightStates)
bool lightStateReceived[MAX_LIGHTS];

// ---------- FUNCTION DECLARATIONS ----------
void setupESPNow();
int  getMaxModeForName(const char* name);
void requestLightStates();
void requestLampPresets(int lightIdx);
void sendColorData();
void buildColorWheel();
void drawColorWheelCanvas();
void syncCwFromColor();
void drawUI();
void drawPresetsPage();
void drawSettingsNav();
void handleSettingsNavTap(int tx, int ty);
void drawLightBar();
void drawAutoReturnBtn();
void drawBatteryIndicator();
void drawLightSelectPage();
void drawLightMainPage();
void drawColorSelectorPage();
void drawSettingsPage();
void drawVerticalSlider(int x, int centerY, int width, int height, int value, int minVal, int maxVal);
void drawSliderLabel(int x, int centerY, int width, int height, const char* label);
void drawModeButtons(int x, int centerY, int width, int height, int value);
void handleTouch();
void switchToLight(uint8_t lightIndex);
void navigateToPage(uint8_t page, bool playSound = false);
void checkAutoReturn();
void checkSleep();
void showLoadingScreen();
void showShutdownAnimation();
void setupAccelerometer();
bool checkAccelerometer();
void HSVtoRGB(uint8_t h, uint8_t s, uint8_t v, uint8_t &r, uint8_t &g, uint8_t &b);

// ---------- AXP2101 POWER KEY (direct I2C — M5.BtnPWR doesn't work on CoreS3 SE) ----------
// Power key IRQ is in register 0x49:
//   bit 1 (0x02) = short press (button held past minimum time, ~400 ms)
//   bit 0 (0x01) = release event  — we ignore this
// Must be read BEFORE M5.update() so M5Unified doesn't clear the bits first.

static inline uint8_t axpRead(uint8_t reg) {
  return M5.In_I2C.readRegister8(0x34, reg, 400000UL);
}
static inline void axpWrite(uint8_t reg, uint8_t val) {
  M5.In_I2C.writeRegister8(0x34, reg, val, 400000UL);
}

static uint32_t powerKeyReadyAt = 0;  // millis() after which power key is active

void initPowerKey() {
  // Enable short-press and release IRQs in IRQEN1 (0x41)
  axpWrite(0x41, axpRead(0x41) | 0x03);
  // Set minimum short-press timer to 0 (shortest possible)
  axpWrite(0x25, 0x00);
  // Clear any stale bits from the boot-press that turned the device on
  axpWrite(0x49, 0x03);
  // Grace period: ignore power key for 4 seconds after boot.
  // The AXP2101 records the boot button press in reg 0x49; without this
  // delay the first loop() iteration would immediately power off again.
  powerKeyReadyAt = millis() + 4000;
}

bool powerKeyShortPressed() {
  uint8_t irq1 = axpRead(0x49);
  // Always clear the bits so they don't accumulate
  if (irq1 & 0x03) {
    axpWrite(0x49, irq1 & 0x03);
  }
  // Ignore presses during the startup grace period
  if (millis() < powerKeyReadyAt) return false;
  return (irq1 & 0x02) != 0;   // bit 1 = short press
}

// ---------- SETUP ----------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Display.setRotation(1);
  M5.Display.setBrightness(128);

  Serial.begin(115200);
  delay(100);
  Serial.println("RGB Remote Starting...");

  // --- Wake reason diagnostics ---
  esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();
  switch (wakeReason) {
    case ESP_SLEEP_WAKEUP_EXT1:   Serial.println("[WAKE] Touch screen (EXT1/GPIO21)"); break;
    case ESP_SLEEP_WAKEUP_TIMER:  Serial.println("[WAKE] Timer"); break;
    case ESP_SLEEP_WAKEUP_EXT0:   Serial.println("[WAKE] EXT0"); break;
    default: Serial.printf("[WAKE] Normal power-on or reset (cause=%d)\n", wakeReason); break;
  }
  Serial.printf("[BATT] Level: %d%%  Charging: %s\n",
                M5.Power.getBatteryLevel(),
                M5.Power.isCharging() ? "yes" : "no");

  WiFi.mode(WIFI_STA);
  Serial.print("MAC Address: ");
  Serial.println(WiFi.macAddress());

  // Initialize light arrays with safe defaults
  for (int i = 0; i < MAX_LIGHTS; i++) {
    lightHue[i] = 30;
    lightSat[i] = 200;
    lightVal[i] = 200;
    lightOn[i] = true;
    lightMode[i] = 0;
    lightSpeed[i] = 128;
    lightFadeout[i] = 10;
    lightMaxMode[i] = 8;
    lightNames[i][0] = '\0';
    lightStateReceived[i] = false;
    lightPresetsLoaded[i] = false;
    lightPresetCount[i] = 0;
  }

  // Setup ESP-NOW (registers callback + broadcast peer)
  setupESPNow();

  // Setup accelerometer for wake
  setupAccelerometer();

  // Send discovery broadcast BEFORE the loading animation so replies
  // arrive while the animation plays — no extra waiting screen needed.
  numLights = 0;
  struct_message disc = {};
  disc.isDiscovery = true;
  esp_now_send(BROADCAST_MAC, (uint8_t*)&disc, sizeof(disc));
  unsigned long discoveryDeadline = millis() + 2000;
  Serial.println("Discovery broadcast sent");

  // Show loading animation (~1.1 s). ESP-NOW callbacks run in a
  // FreeRTOS Wi-Fi task so discovery replies are collected in parallel.
  showLoadingScreen();

  // Wait out any remaining discovery time (usually only a few hundred ms)
  while (millis() < discoveryDeadline) delay(20);
  Serial.printf("Discovery complete: %d device(s) found\n", numLights);

  // Read current state from discovered lights
  if (numLights > 0) {
    requestLightStates();
  }

  // Create color wheel sprites (320 × WHEEL_HEIGHT = 320 × 176 px)
  canvas.createSprite(320, WHEEL_HEIGHT);
  wheelBg.createSprite(320, WHEEL_HEIGHT);

  // Draw initial UI (Light Select page)
  currentPage = PAGE_LIGHT_SELECT;
  drawUI();
  
  // Send initial color
  // sendColorData(); // Commented out - don't send on startup
  
  lastTouchTime = millis();
  
  // Initialize charging state tracking
  lastChargingState = M5.Power.isCharging();
  
  initPowerKey();
  Serial.println("Ready!");
}

// ---------- MAIN LOOP ----------
void loop() {
  // Power button: check BEFORE M5.update() so M5Unified can't clear the IRQ bits first.
  // Hold the side button for ~0.5 s → plays shutdown animation then powers off.
  if (powerKeyShortPressed()) {
    Serial.println("Power button — shutting down");
    showShutdownAnimation();   // blocking; calls powerOff() at end
  }

  M5.update();

  // Check for USB/charging state changes
  bool currentChargingState = M5.Power.isCharging();
  if (currentChargingState != lastChargingState) {
    lastChargingState = currentChargingState;
    Serial.println(currentChargingState ? "USB connected/Charging" : "USB disconnected/On battery");
    if (IS_SETTINGS_PAGE(currentPage)) drawSettingsNav();
    drawBatteryIndicator();
  }
  
  // Check for auto-return to previous page
  checkAutoReturn();
  
  bool isTouching = (M5.Touch.getCount() > 0);
  
  // Handle touch input - only on fresh touch (not while holding)
  if (isTouching) {
    if (!displayOn) {
      // Wake display - just turn on backlight, no redraw
      M5.Display.setBrightness(128);
      displayOn = true;
    }
    if (!wasTouching) {
      // Fresh touch down - handle it
      handleTouch();
    } else if ((currentPage == PAGE_COLOR_SELECTOR || currentPage == PAGE_SETTINGS) && !ignoreFirstTouch) {
      // Color selector and settings sliders need continuous drag updates
      handleTouch();
    }
    lastTouchTime = millis();
  }
  
  // Detect touch release
  if (wasTouching && !isTouching) {
    // Play click sound on release only if slider was actually dragged
    if (currentPage == PAGE_SETTINGS && sliderWasUsed) {
      M5.Speaker.tone(4000, 20);
    }
    
    wasTouching = false;
    ignoreFirstTouch = false;  // Clear flag on finger lift
    colorWheelActive = false;  // Reset color wheel flag
    sliderWasUsed = false;     // Reset slider flag
    // Check if we should unlock touches
    unsigned long now = millis();
    if (touchLocked && now >= ignoreUntilTime) {
      touchLocked = false;
      Serial.println("=== UNLOCKING TOUCHES ===");
    }
  }
  
  // Check for sleep conditions
  checkSleep();

  // --- Periodic battery/state diagnostics (every 60 seconds) ---
  static unsigned long lastBattLog = 0;
  if (millis() - lastBattLog >= 60000) {
    lastBattLog = millis();
    Serial.printf("[BATT] %d%%  Charging: %s  Display: %s  TimeSinceTouch: %lus\n",
                  M5.Power.getBatteryLevel(),
                  M5.Power.isCharging() ? "yes" : "no",
                  displayOn ? "on" : "off",
                  (millis() - lastTouchTime) / 1000);
  }

  delay(50);
}

// ---------- ESP-NOW RECEIVE CALLBACK ----------
// Called for both discovery replies and state responses from lights.
void OnDataRecvFromLight(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
  const uint8_t *mac = recv_info->src_addr;

  // ── Preset packet: lamp sends its preset list ──────────────────────────
  if (len == (int)sizeof(preset_packet)) {
    const preset_packet *pp = (preset_packet*)data;
    if (pp->isPresetPacket) {
      for (int i = 0; i < numLights; i++) {
        if (memcmp(mac, lightMacAddresses[i], 6) == 0) {
          uint8_t cnt = (pp->count < MAX_PRESETS) ? pp->count : MAX_PRESETS;
          lightPresetCount[i] = cnt;
          for (int j = 0; j < cnt; j++) lightPresets[i][j] = pp->slots[j];
          lightPresetsLoaded[i] = true;
          Serial.printf("Presets received from Light %d (%s): %d presets\n", i + 1, lightNames[i], cnt);
          break;
        }
      }
      return;
    }
  }

  if (len < (int)sizeof(struct_message)) return;
  struct_message *msg = (struct_message*)data;

  // ── Discovery reply: light announces itself with its name ──────────────
  if (msg->isDiscovery && !msg->requestState) {
    if (numLights >= MAX_LIGHTS) return;

    // Ignore if we already know this device (duplicate reply)
    for (int i = 0; i < numLights; i++) {
      if (memcmp(mac, lightMacAddresses[i], 6) == 0) return;
    }

    int idx = numLights;
    memcpy(lightMacAddresses[idx], mac, 6);
    strncpy(lightNames[idx], msg->deviceName, 15);
    lightNames[idx][15] = '\0';
    lightMaxMode[idx] = getMaxModeForName(msg->deviceName);

    // Register this light as an ESP-NOW peer so we can send to it
    esp_now_peer_info_t newPeer = {};
    memcpy(newPeer.peer_addr, mac, 6);
    newPeer.channel = 0;
    newPeer.encrypt = false;
    esp_now_add_peer(&newPeer);

    numLights++;
    Serial.printf("Discovered: \"%s\" (%02X:%02X:%02X:%02X:%02X:%02X) maxMode=%d\n",
                  lightNames[idx],
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                  lightMaxMode[idx]);
    return;
  }

  // ── State response: light replies to requestLightStates() ──────────────
  if (msg->requestState) return;  // Safety: ignore stray requests

  for (int i = 0; i < numLights; i++) {
    if (memcmp(mac, lightMacAddresses[i], 6) == 0) {
      lightHue[i]     = msg->h;
      lightSat[i]     = msg->s;
      lightVal[i]     = msg->v;
      lightOn[i]      = msg->power;
      lightMode[i]    = msg->mode;
      lightSpeed[i]   = msg->speed;
      lightFadeout[i] = msg->fadeout;
      lightStateReceived[i] = true;
      Serial.printf("State received from Light %d (%s): H=%d S=%d V=%d ON=%d Mode=%d\n",
                    i + 1, lightNames[i], msg->h, msg->s, msg->v, msg->power, msg->mode);
      break;
    }
  }
}

// ---------- ESP-NOW SETUP ----------
void setupESPNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  Serial.println("ESP-NOW Initialized");

  // Register receive callback (handles discovery replies and state responses)
  esp_now_register_recv_cb(OnDataRecvFromLight);

  // Register broadcast peer — used to send discovery packets to all devices
  esp_now_peer_info_t broadcastPeer = {};
  memcpy(broadcastPeer.peer_addr, BROADCAST_MAC, 6);
  broadcastPeer.channel = 0;
  broadcastPeer.encrypt = false;
  if (esp_now_add_peer(&broadcastPeer) != ESP_OK) {
    Serial.println("Failed to add broadcast peer");
  } else {
    Serial.println("Broadcast peer added");
  }
}

// ---------- DEVICE NAME → MAXMODE LOOKUP ----------
int getMaxModeForName(const char* name) {
  for (int i = 0; i < NUM_KNOWN_DEVICES; i++) {
    if (strcmp(name, knownDevices[i].name) == 0) return knownDevices[i].maxMode;
  }
  return 8;  // Safe default for unknown devices
}

// ---------- REQUEST LIGHT STATES ----------
// Sends a state-request ping to each discovered light and waits up to 2s for responses.
void requestLightStates() {
  if (numLights == 0) return;
  Serial.println("Requesting state from all lights...");

  // Reset receive flags
  for (int i = 0; i < numLights; i++) lightStateReceived[i] = false;

  // Send request to each light (lights ignore h/s/v/etc when requestState=true)
  struct_message req = {};
  req.requestState = true;
  for (int i = 0; i < numLights; i++) {
    esp_err_t result = esp_now_send(lightMacAddresses[i], (uint8_t*)&req, sizeof(req));
    Serial.printf("State request sent to Light %d (%s): %s\n", i + 1, lightNames[i],
                  result == ESP_OK ? "OK" : "FAIL");
  }

  // Wait up to 2 seconds for all lights to respond
  unsigned long deadline = millis() + 2000;
  while (millis() < deadline) {
    bool allDone = true;
    for (int i = 0; i < numLights; i++) {
      if (!lightStateReceived[i]) { allDone = false; break; }
    }
    if (allDone) break;
    delay(20);
  }

  // Retry once for any lights that didn't reply on the first attempt
  bool anyMissed = false;
  for (int i = 0; i < numLights; i++) {
    if (!lightStateReceived[i]) {
      anyMissed = true;
      esp_err_t result = esp_now_send(lightMacAddresses[i], (uint8_t*)&req, sizeof(req));
      Serial.printf("Retry state request to Light %d (%s): %s\n", i + 1, lightNames[i],
                    result == ESP_OK ? "OK" : "FAIL");
    }
  }
  if (anyMissed) {
    deadline = millis() + 2000;
    while (millis() < deadline) {
      bool allDone = true;
      for (int i = 0; i < numLights; i++) {
        if (!lightStateReceived[i]) { allDone = false; break; }
      }
      if (allDone) break;
      delay(20);
    }
  }

  // Log final results
  for (int i = 0; i < numLights; i++) {
    if (lightStateReceived[i]) {
      Serial.printf("Light %d (%s): state updated from device\n", i + 1, lightNames[i]);
    } else {
      Serial.printf("Light %d (%s): no response — keeping defaults\n", i + 1, lightNames[i]);
    }
  }

  // Sync active light's working values
  if (currentLight < numLights) {
    currentHue     = lightHue[currentLight];
    currentSat     = lightSat[currentLight];
    currentVal     = lightVal[currentLight];
    currentOn      = lightOn[currentLight];
    currentMode    = lightMode[currentLight];
    currentSpeed   = lightSpeed[currentLight];
    currentFadeout = lightFadeout[currentLight];
  }
}

// ---------- REQUEST LAMP PRESETS ----------
// Sends a preset-request to one lamp and waits up to 1 s for the reply.
void requestLampPresets(int idx) {
  if (idx < 0 || idx >= numLights) return;
  Serial.printf("Requesting presets from Light %d (%s)...\n", idx + 1, lightNames[idx]);

  struct_message req = {};
  req.requestPresets = true;
  esp_err_t result = esp_now_send(lightMacAddresses[idx], (uint8_t*)&req, sizeof(req));
  if (result != ESP_OK) {
    Serial.printf("Preset request send failed for Light %d\n", idx + 1);
    return;
  }

  unsigned long deadline = millis() + 1000;
  while (millis() < deadline && !lightPresetsLoaded[idx]) delay(20);

  if (lightPresetsLoaded[idx]) {
    Serial.printf("Presets loaded for Light %d (%d presets)\n", idx + 1, lightPresetCount[idx]);
  } else {
    Serial.printf("Preset timeout for Light %d\n", idx + 1);
  }
}

// ---------- SEND COLOR DATA ----------
void sendColorData() {
  // Always flush current light's state into arrays before saving
  lightHue[currentLight]     = currentHue;
  lightSat[currentLight]     = currentSat;
  lightVal[currentLight]     = currentVal;
  lightOn[currentLight]      = currentOn;
  lightMode[currentLight]    = currentMode;
  lightSpeed[currentLight]   = currentSpeed;
  lightFadeout[currentLight] = currentFadeout;
  
  outgoingData.h            = currentHue;
  outgoingData.s            = currentSat;
  outgoingData.v            = currentVal;
  outgoingData.power        = currentOn;
  outgoingData.mode         = currentMode;
  outgoingData.speed        = currentSpeed;
  outgoingData.fadeout      = currentFadeout;
  outgoingData.requestState = false;  // never request state on a color send
  outgoingData.isDiscovery  = false;  // never a discovery packet
  
  // Send to currently selected light
  esp_err_t result = esp_now_send(lightMacAddresses[currentLight], (uint8_t *) &outgoingData, sizeof(outgoingData));
  
  if (result == ESP_OK) {
    Serial.printf("Sent to Light %d: H=%d S=%d V=%d Power=%d Mode=%d\n", currentLight + 1, currentHue, currentSat, currentVal, currentOn, 0);
  } else {
    Serial.printf("Error sending to Light %d\n", currentLight + 1);
  }
}

// ---------- DRAW UI ----------
// ---------- DRAW PRESETS PAGE ----------
void drawPresetsPage() {
  // Load presets from lamp if not yet fetched
  if (!lightPresetsLoaded[currentLight]) {
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextSize(1);
    M5.Display.drawString("Loading...", 160, 100);
    requestLampPresets(currentLight);
    M5.Display.fillScreen(TFT_BLACK);
    drawSettingsNav();
    drawBatteryIndicator();
  }

  const int sqW = 60, cols = 4;
  const int spacing = 10;
  const int startX = 25;
  const int rowY[2] = { 26, 116 };

  uint8_t count = lightPresetCount[currentLight];

  for (int i = 0; i < 8; i++) {
    int col = i % cols;
    int row = i / cols;
    int x = startX + col * (sqW + spacing);
    int y = rowY[row];

    if (i < count) {
      uint8_t r, g, b;
      HSVtoRGB(lightPresets[currentLight][i].h,
               lightPresets[currentLight][i].s,
               lightPresets[currentLight][i].v, r, g, b);
      M5.Display.fillRect(x, y, sqW, sqW, M5.Display.color565(r, g, b));
    } else {
      M5.Display.fillRect(x, y, sqW, sqW, M5.Display.color565(30, 30, 30));
    }

    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor(M5.Display.color565(100, 100, 100), TFT_BLACK);
    M5.Display.setTextDatum(top_center);
    M5.Display.setTextSize(1);
    const char* label = (i < count) ? lightPresets[currentLight][i].name : "";
    M5.Display.drawString(label, x + sqW / 2, y + sqW + 10);
  }
}

// ---------- DRAW SETTINGS NAV BAR ----------
void drawSettingsNav() {
  M5.Display.startWrite();
  M5.Display.fillRect(0, SNAV_Y, 320, SNAV_H, TFT_BLACK);

  int cy = SNAV_Y + SNAV_H / 2;

  // Left triangle (always active — circular)
  M5.Display.fillTriangle(
    SNAV_BTN_W / 2 - 6, cy,
    SNAV_BTN_W / 2 + 6, cy - 7,
    SNAV_BTN_W / 2 + 6, cy + 7,
    SNAV_COL_ACT);

  // Right triangle (always active — circular)
  M5.Display.fillTriangle(
    320 - SNAV_BTN_W / 2 + 6, cy,
    320 - SNAV_BTN_W / 2 - 6, cy - 7,
    320 - SNAV_BTN_W / 2 - 6, cy + 7,
    SNAV_COL_ACT);

  // Page name
  const char* name = settingsPageNames[currentPage - PAGE_PRESETS];
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextColor(SNAV_COL_ACT, TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(1);
  M5.Display.drawString(name, SNAV_TITLE_X + SNAV_TITLE_W / 2, cy);

  M5.Display.endWrite();
}

// ---------- HANDLE SETTINGS NAV TAP ----------
void handleSettingsNavTap(int tx, int ty) {
  if (tx < SNAV_BTN_W) {
    // Left — previous, wrap around
    uint8_t next = (currentPage <= PAGE_PRESETS) ? PAGE_SETTINGS : currentPage - 1;
    navigateToPage(next, true);
  } else if (tx >= 320 - SNAV_BTN_W) {
    // Right — next, wrap around
    uint8_t next = (currentPage >= PAGE_SETTINGS) ? PAGE_PRESETS : currentPage + 1;
    navigateToPage(next, true);
  }
}

void drawUI() {
  M5.Display.fillScreen(TFT_BLACK);
  
  // Draw page-specific content
  switch (currentPage) {
    case PAGE_LIGHT_SELECT:   drawLightSelectPage();    break;
    case PAGE_LIGHT_MAIN:     drawLightMainPage();      break;
    case PAGE_PRESETS:        drawPresetsPage();        break;
    case PAGE_COLOR_SELECTOR: drawColorSelectorPage();  break;
    case PAGE_SETTINGS:       drawSettingsPage();       break;
  }

  if (currentPage != PAGE_LOADING) {
    if (IS_SETTINGS_PAGE(currentPage)) drawSettingsNav();
    drawBatteryIndicator();  // Always top-right on all pages
  }
}

// ---------- DRAW LIGHT BAR ----------
void drawLightBar() {
  if (numLights == 0) return;
  int barW = 320 / numLights;  // Width of each light's section

  for (int i = 0; i < numLights; i++) {
    int x = i * barW;

    // Get the color for this light
    uint8_t r, g, b;
    HSVtoRGB(lightHue[i], lightSat[i], lightVal[i], r, g, b);
    uint16_t color = M5.Display.color565(r, g, b);

    // Draw rectangle for this light
    M5.Display.fillRect(x, LIGHT_BAR_Y, barW, LIGHT_BAR_HEIGHT, color);

    // Draw white indicator line under active light (3px thick at bottom of bar)
    if (i == currentLight) {
      for (int line = 0; line < LIGHT_BAR_INDICATOR_HEIGHT; line++) {
        M5.Display.drawLine(x, LIGHT_BAR_Y + LIGHT_BAR_HEIGHT - LIGHT_BAR_INDICATOR_HEIGHT + line,
                            x + barW - 1, LIGHT_BAR_Y + LIGHT_BAR_HEIGHT - LIGHT_BAR_INDICATOR_HEIGHT + line,
                            TFT_WHITE);
      }
    }

    // Draw thin separator line between lights
    if (i < numLights - 1) {
      int lineX = (i + 1) * barW;
      M5.Display.drawLine(lineX, LIGHT_BAR_Y, lineX, LIGHT_BAR_Y + LIGHT_BAR_HEIGHT, TFT_BLACK);
    }
  }

  // Draw the auto-return toggle button on top
  drawAutoReturnBtn();
}

// ---------- DRAW AUTO-RETURN TOGGLE BUTTON ----------
// Small right-pointing triangle in the top-left corner of the light bar.
// White = auto-return ON  |  Dark grey = auto-return OFF
void drawAutoReturnBtn() {
  // Black border triangle
  M5.Display.fillTriangle(2, 3, 2, 22, 19, 12, TFT_BLACK);
  // Filled inner triangle
  uint16_t c = autoReturnEnabled ? TFT_WHITE : M5.Display.color565(70, 70, 70);
  M5.Display.fillTriangle(4, 6, 4, 19, 16, 12, c);
}

// ---------- DRAW COLOR WHEEL ----------
// ---------- COLOR WHEEL CANVAS (pre-rendered, flicker-free) ----------

// Pre-render HSV gradient into wheelBg sprite (runs once on first page visit)
void buildColorWheel() {
  for (int y = 0; y < WHEEL_HEIGHT; y++) {
    float yNorm = (float)y / (WHEEL_HEIGHT - 1);
    uint8_t sat, val;
    if (yNorm < 0.5f) {
      sat = (uint8_t)(yNorm * 2.0f * 255);
      val = 255;
    } else {
      sat = 255;
      val = (uint8_t)((1.0f - yNorm) * 2.0f * 255);
    }
    for (int x = 0; x < WHEEL_WIDTH; x++) {
      uint8_t hue = (uint8_t)((float)x / (WHEEL_WIDTH - 1) * 255);
      uint8_t r, g, b;
      HSVtoRGB(hue, sat, val, r, g, b);
      wheelBg.drawPixel(x, y, wheelBg.color565(r, g, b));
    }
  }
  wheelReady = true;
}

// Blit background + cursor into canvas, then DMA-push to LCD
void drawColorWheelCanvas() {
  if (!wheelReady) buildColorWheel();
  wheelBg.pushSprite(&canvas, 0, 0);

  int cx = constrain((int)(cwX * (WHEEL_WIDTH  - 1)), 0, WHEEL_WIDTH  - 1);
  int cy = constrain((int)(cwY * (WHEEL_HEIGHT - 1)), 0, WHEEL_HEIGHT - 1);

  uint8_t r, g, b;
  HSVtoRGB(currentHue, currentSat, currentVal, r, g, b);
  uint16_t pickedCol = canvas.color565(r, g, b);

  canvas.fillCircle(cx, cy, 14, pickedCol);   // Larger cursor (radius 14)
  canvas.pushSprite(WHEEL_X, WHEEL_Y);        // DMA push to LCD
}

// Initialise cwX / cwY from the current HSV values so the cursor lands
// on the right spot when entering the color selector page
void syncCwFromColor() {
  cwX = (float)currentHue / 255.0f;
  float yNorm;
  if (currentSat < 255) {
    yNorm = (currentSat / 255.0f) * 0.5f;        // Top half (white → colour)
  } else {
    yNorm = 1.0f - (currentVal / 255.0f) * 0.5f; // Bottom half (colour → black)
  }
  cwY = yNorm;
}

// ---------- DRAW BATTERY INDICATOR ----------
void drawBatteryIndicator() {
  // Get battery voltage and percentage
  int batteryLevel = M5.Power.getBatteryLevel(); // Returns 0-100
  bool isCharging = M5.Power.isCharging();       // Check if charging
  
  // Determine color based on battery and power state
  uint16_t batteryColor;
  if (isCharging) {
    // Blue when charging
    batteryColor = M5.Display.color565(0, 150, 255);
  } else if (batteryLevel > 60) {
    // Green (full)
    batteryColor = M5.Display.color565(0, 255, 0);
  } else if (batteryLevel > 30) {
    // Orange (medium)
    batteryColor = M5.Display.color565(255, 165, 0);
  } else {
    // Red (low)
    batteryColor = M5.Display.color565(255, 0, 0);
  }
  
  // Draw battery indicator circle
  M5.Display.fillCircle(BATTERY_INDICATOR_X, BATTERY_INDICATOR_Y, BATTERY_INDICATOR_RADIUS, batteryColor);
}

// ---------- SWITCH TO LIGHT ----------
void switchToLight(uint8_t lightIndex) {
  if (lightIndex >= (uint8_t)numLights) return;

  // Flush current light's working state back to its array slot
  lightHue[currentLight]     = currentHue;
  lightSat[currentLight]     = currentSat;
  lightVal[currentLight]     = currentVal;
  lightOn[currentLight]      = currentOn;
  lightMode[currentLight]    = currentMode;
  lightSpeed[currentLight]   = currentSpeed;
  lightFadeout[currentLight] = currentFadeout;

  // Switch to new light
  currentLight = lightIndex;

  // Load new light's values
  currentHue     = lightHue[currentLight];
  currentSat     = lightSat[currentLight];
  currentVal     = lightVal[currentLight];
  currentOn      = lightOn[currentLight];
  currentMode    = lightMode[currentLight];
  currentSpeed   = lightSpeed[currentLight];
  currentFadeout = lightFadeout[currentLight];

  Serial.printf("Switched to Light %d (%s) H=%d S=%d V=%d ON=%d\n",
                currentLight + 1, lightNames[currentLight],
                currentHue, currentSat, currentVal, currentOn);

  // Update light bar to show active light indicator
  drawLightBar();
  drawBatteryIndicator();

  // Send color to newly selected light
  sendColorData();
}

// ---------- DRAW LIGHT SELECT PAGE ----------
void drawLightSelectPage() {
  if (numLights == 0) {
    // No lights found — show message only
    M5.Display.setFont(&fonts::Font4);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString("No devices", 160, 100);
    M5.Display.drawString("found", 160, 140);
    return;
  }

  int squareSize = 80;
  int totalWidth = (squareSize * numLights) + (BUTTON_SPACING * (numLights - 1));
  int startX = (320 - totalWidth) / 2;
  int y = (240 - squareSize) / 2;

  for (int i = 0; i < numLights; i++) {
    int x = startX + (i * (squareSize + BUTTON_SPACING));

    uint8_t r, g, b;
    HSVtoRGB(lightHue[i], lightSat[i], lightVal[i], r, g, b);
    uint16_t color = M5.Display.color565(r, g, b);
    M5.Display.fillRect(x, y, squareSize, squareSize, color);

    // If OFF, overlay black triangle on top-left diagonal
    if (!lightOn[i]) {
      for (int py = 0; py < squareSize; py++) {
        for (int px = 0; px < squareSize; px++) {
          if (px + py < squareSize) {
            M5.Display.drawPixel(x + px, y + py, TFT_BLACK);
          }
        }
      }
    }

    // Draw label below square
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor(M5.Display.color565(100, 100, 100), TFT_BLACK);
    M5.Display.setTextDatum(top_center);
    M5.Display.setTextSize(1);
    M5.Display.drawString(lightNames[i], x + squareSize / 2, y + squareSize + 10);
  }
}

// ---------- DRAW LIGHT MAIN PAGE ----------
void drawLightMainPage() {
  // No title - just 3 squares in a line with labels
  
  // Calculate square dimensions
  int squareSize = 80;  // Size of each square
  int totalWidth = (squareSize * 3) + (BUTTON_SPACING * 2);
  int startX = (320 - totalWidth) / 2;  // Center horizontally
  int y = (240 - squareSize) / 2;  // Center vertically
  
  const char* labels[3] = {"On/Off", "Color", "Settings"};
  
  for (int i = 0; i < 3; i++) {
    int x = startX + (i * (squareSize + BUTTON_SPACING));
    
    if (i == 0) {
      // ON/OFF button - diagonal pattern: black top-left, white bottom-right
      // First fill with white
      M5.Display.fillRect(x, y, squareSize, squareSize, TFT_WHITE);
      
      // Fill top-left diagonal half with black
      for (int py = 0; py < squareSize; py++) {
        for (int px = 0; px < squareSize; px++) {
          // Top-left diagonal: if px + py < squareSize
          if (px + py < squareSize) {
            M5.Display.drawPixel(x + px, y + py, TFT_BLACK);
          }
        }
      }
      
    } else if (i == 1) {
      // Color button - always show exact color, overlay black triangle if OFF
      uint8_t r, g, b;
      HSVtoRGB(currentHue, currentSat, currentVal, r, g, b);
      uint16_t color = M5.Display.color565(r, g, b);
      M5.Display.fillRect(x, y, squareSize, squareSize, color);
      
      // If OFF, overlay black triangle on top-left diagonal
      if (!currentOn) {
        for (int py = 0; py < squareSize; py++) {
          for (int px = 0; px < squareSize; px++) {
            if (px + py < squareSize) {
              M5.Display.drawPixel(x + px, y + py, TFT_BLACK);
            }
          }
        }
      }
      
    } else {
      // Settings button - filled white square
      M5.Display.fillRect(x, y, squareSize, squareSize, TFT_WHITE);
    }
    
    // Draw label below square
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor(M5.Display.color565(100, 100, 100));  // Dark grey
    M5.Display.setTextDatum(top_center);
    M5.Display.setTextSize(1);
    M5.Display.drawString(labels[i], x + squareSize/2, y + squareSize + 10);
  }
}

// ---------- DRAW COLOR SELECTOR PAGE ----------
void drawColorSelectorPage() {
  syncCwFromColor();
  drawColorWheelCanvas();
}

// ---------- DRAW SETTINGS PAGE ----------
void drawSettingsPage() {
  // 3 vertical sliders + 1 mode +/- widget (80px wide, same as squares on other pages)

  int sliderWidth  = 60;
  int modeWidth    = sliderWidth;  // Same width as sliders
  int sliderHeight = 150;
  int spacing      = 10;
  int totalWidth   = (sliderWidth * 4) + (spacing * 3);
  int startX       = (320 - totalWidth) / 2;
  int centerY      = (WHEEL_Y + SNAV_Y) / 2 - 15;  // 106 px — centred in content area, shifted up
  int modeX        = startX + (sliderWidth + spacing) * 3;

  // Brightness (0-255)
  drawVerticalSlider(startX, centerY, sliderWidth, sliderHeight, currentVal, 0, 255);
  drawSliderLabel(startX, centerY, sliderWidth, sliderHeight, "Bright");

  // Speed (0-255)
  drawVerticalSlider(startX + (sliderWidth + spacing), centerY, sliderWidth, sliderHeight, currentSpeed, 0, 255);
  drawSliderLabel(startX + (sliderWidth + spacing), centerY, sliderWidth, sliderHeight, "Speed");

  // Fadeout (0-255)
  drawVerticalSlider(startX + (sliderWidth + spacing) * 2, centerY, sliderWidth, sliderHeight, currentFadeout, 0, 255);
  drawSliderLabel(startX + (sliderWidth + spacing) * 2, centerY, sliderWidth, sliderHeight, "Fade");

  // Mode (0-7): two 80×80 squares (+/-), number shown as label below
  drawModeButtons(modeX, centerY, modeWidth, sliderHeight, currentMode);
  char modeLabel[8];
  sprintf(modeLabel, "Mode %d", currentMode);
  drawSliderLabel(modeX, centerY, modeWidth, sliderHeight, modeLabel);
}

// Helper function to draw vertical slider with fill from bottom
void drawVerticalSlider(int x, int centerY, int width, int height, int value, int minVal, int maxVal) {
  int y = centerY - height/2;
  
  // Draw dark grey background (no border)
  uint16_t darkGrey = M5.Display.color565(40, 40, 40);
  M5.Display.fillRect(x, y, width, height, darkGrey);
  
  // Calculate fill percentage
  float fillPercent = (float)(value - minVal) / (float)(maxVal - minVal);
  int fillHeight = (int)(height * fillPercent);
  
  // Fill from bottom up with white (always draw at least 1 pixel if value > minVal)
  if (fillHeight > 0) {
    M5.Display.fillRect(x, y + height - fillHeight, width, fillHeight, TFT_WHITE);
  } else if (value > minVal) {
    // Draw at least 1 pixel for non-zero values
    M5.Display.fillRect(x, y + height - 1, width, 1, TFT_WHITE);
  }
}

// Helper function to draw label below slider
void drawSliderLabel(int x, int centerY, int width, int height, const char* label) {
  int y = centerY + height/2 + 10;  // 10px below slider

  M5.Display.setFont(&fonts::Font0);  // Small sans-serif font
  M5.Display.setTextColor(M5.Display.color565(100, 100, 100));  // Dark grey
  M5.Display.setTextDatum(top_center);
  M5.Display.setTextSize(1);
  M5.Display.drawString(label, x + width/2, y);
}

// Draw mode +/- buttons: two square white buttons (width × width).
// "+" top-aligned with sliders, "−" bottom-aligned with sliders.
void drawModeButtons(int x, int centerY, int width, int height, int value) {
  int btnH   = width;                   // Square: 60×60
  int top    = centerY - height / 2;   // Top of slider area    (y=40)
  int bottom = centerY + height / 2;   // Bottom of slider area (y=200)

  // Clear entire column so the gap between buttons is always black
  M5.Display.fillRect(x, top, width, height, TFT_BLACK);

  // "+" button — top-aligned
  M5.Display.fillRect(x, top, width, btnH, TFT_WHITE);
  M5.Display.setFont(&fonts::Font4);
  M5.Display.setTextColor(TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(1);
  M5.Display.drawString("+", x + width/2, top + btnH/2);

  // "−" button — bottom-aligned
  int minusBtnY = bottom - btnH;
  M5.Display.fillRect(x, minusBtnY, width, btnH, TFT_WHITE);
  M5.Display.setFont(&fonts::Font4);
  M5.Display.setTextColor(TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(1);
  M5.Display.drawString("-", x + width/2, minusBtnY + btnH/2);
}

// ---------- NAVIGATE TO PAGE ----------
void navigateToPage(uint8_t page, bool playSound) {
  // Play subtle click sound only if requested (button press, not auto-return)
  if (playSound) {
    M5.Speaker.tone(4000, 20);  // 4kHz for 20ms - subtle click
  }
  
  previousPage = currentPage;
  currentPage = page;
  lastPageChangeTime = millis();
  ignoreFirstTouch = true;  // Ignore the first touch after arriving on new page
  drawUI();
}

// ---------- CHECK AUTO RETURN ----------
void checkAutoReturn() {
  // Feature can be toggled via the triangle button in the top-left of the light bar
  if (!autoReturnEnabled) return;

  // Only auto-return if not on Light Select page
  if (currentPage == PAGE_LIGHT_SELECT) {
    return;
  }
  
  unsigned long now = millis();
  if (now - lastPageChangeTime >= AUTO_RETURN_TIMEOUT) {
    // Return to previous page, or Light Select if at main
    if (currentPage == PAGE_LIGHT_MAIN) {
      navigateToPage(PAGE_LIGHT_SELECT);
    } else {
      navigateToPage(PAGE_LIGHT_MAIN);
    }
  }
}

// ---------- HANDLE TOUCH ----------
// ---------- HANDLE TOUCH ----------
void handleTouch() {
  auto touch = M5.Touch.getDetail();
  int touchX = touch.x;
  int touchY = touch.y;
  
  // Single debounce check for ALL interactions
  unsigned long now = millis();
  if (now - lastInteractionTime < DEBOUNCE) {
    wasTouching = true;
    return;
  }
  
  // Reset auto-return timer on any touch
  lastPageChangeTime = now;

  // Auto-return toggle button — top-left corner of the light bar (all pages)
  if (touchX < AUTO_BTN_HIT && touchY < AUTO_BTN_HIT) {
    autoReturnEnabled = !autoReturnEnabled;
    lastInteractionTime = now;
    drawAutoReturnBtn();
    wasTouching = true;
    return;
  }

  // Settings sub-pages nav bar tap
  if (IS_SETTINGS_PAGE(currentPage) && touchY >= SNAV_Y) {
    lastInteractionTime = now;
    handleSettingsNavTap(touchX, touchY);
    wasTouching = true;
    return;
  }

  switch (currentPage) {
    case PAGE_LIGHT_SELECT:
      {
        if (numLights == 0) break;  // Nothing to tap

        int squareSize = 80;
        int totalWidth = (squareSize * numLights) + (BUTTON_SPACING * (numLights - 1));
        int startX = (320 - totalWidth) / 2;
        int y = (240 - squareSize) / 2;

        for (int i = 0; i < numLights; i++) {
          int x = startX + (i * (squareSize + BUTTON_SPACING));

          if (touchX >= x && touchX <= x + squareSize &&
              touchY >= y && touchY <= y + squareSize) {
            currentLight = i;
            currentHue     = lightHue[currentLight];
            currentSat     = lightSat[currentLight];
            currentVal     = lightVal[currentLight];
            currentOn      = lightOn[currentLight];
            currentMode    = lightMode[currentLight];
            currentSpeed   = lightSpeed[currentLight];
            currentFadeout = lightFadeout[currentLight];
            // Proactively request presets so they're ready when the user opens Presets
            if (!lightPresetsLoaded[currentLight]) {
              struct_message req = {};
              req.requestPresets = true;
              esp_now_send(lightMacAddresses[currentLight], (uint8_t*)&req, sizeof(req));
            }
            lastInteractionTime = now;
            navigateToPage(PAGE_LIGHT_MAIN, true);
            wasTouching = true;
            return;
          }
        }
      }
      break;
      
    case PAGE_LIGHT_MAIN:
      {
        int squareSize = 80;
        int totalWidth = (squareSize * 3) + (BUTTON_SPACING * 2);
        int startX = (320 - totalWidth) / 2;
        int y = (240 - squareSize) / 2;
        
        for (int i = 0; i < 3; i++) {
          int x = startX + (i * (squareSize + BUTTON_SPACING));
          
          if (touchX >= x && touchX <= x + squareSize &&
              touchY >= y && touchY <= y + squareSize) {
            
            lastInteractionTime = now;
            
            if (i == 0) {
              // ON/OFF toggle
              M5.Speaker.tone(4000, 20);  // Click sound
              currentOn = !currentOn;
              lightOn[currentLight] = currentOn;
              sendColorData();
              
              // Redraw only the middle color button
              int colorX = startX + (1 * (squareSize + BUTTON_SPACING));
              uint8_t r, g, b;
              HSVtoRGB(currentHue, currentSat, currentVal, r, g, b);
              uint16_t color = M5.Display.color565(r, g, b);
              M5.Display.fillRect(colorX, y, squareSize, squareSize, color);
              if (!currentOn) {
                for (int py = 0; py < squareSize; py++) {
                  for (int px = 0; px < squareSize; px++) {
                    if (px + py < squareSize) {
                      M5.Display.drawPixel(colorX + px, y + py, TFT_BLACK);
                    }
                  }
                }
              }
            } else if (i == 1) {
              navigateToPage(PAGE_COLOR_SELECTOR, true);
            } else if (i == 2) {
              navigateToPage(PAGE_PRESETS, true);  // Enter settings group at Presets
            }
            
            wasTouching = true;
            return;
          }
        }
      }
      break;
      
    case PAGE_PRESETS:
      {
        const int sqW = 60, cols = 4;
        const int spacing = 10;
        const int startX = 25;
        const int rowY[2] = { 26, 116 };

        uint8_t count = lightPresetCount[currentLight];
        for (int i = 0; i < count; i++) {
          int col = i % cols;
          int row = i / cols;
          int x = startX + col * (sqW + spacing);
          int y = rowY[row];

          if (touchX >= x && touchX < x + sqW &&
              touchY >= y && touchY < y + sqW) {
            currentHue     = lightPresets[currentLight][i].h;
            currentSat     = lightPresets[currentLight][i].s;
            currentVal     = lightPresets[currentLight][i].v;
            currentOn      = lightPresets[currentLight][i].power;
            currentMode    = lightPresets[currentLight][i].mode;
            currentSpeed   = lightPresets[currentLight][i].speed;
            currentFadeout = lightPresets[currentLight][i].fadeout;
            lightHue[currentLight]     = currentHue;
            lightSat[currentLight]     = currentSat;
            lightVal[currentLight]     = currentVal;
            lightOn[currentLight]      = currentOn;
            lightMode[currentLight]    = currentMode;
            lightSpeed[currentLight]   = currentSpeed;
            lightFadeout[currentLight] = currentFadeout;
            lastInteractionTime = now;
            M5.Speaker.tone(4000, 20);
            sendColorData();
            wasTouching = true;
            return;
          }
        }
      }
      break;

    case PAGE_COLOR_SELECTOR:
      {
        // Color wheel does not use debounce (needs continuous dragging)
        if (touchX >= WHEEL_X && touchX < WHEEL_X + WHEEL_WIDTH &&
            touchY >= WHEEL_Y && touchY < WHEEL_Y + WHEEL_HEIGHT) {

          // If light is off, turn it on when user selects a color
          if (!currentOn) {
            currentOn = true;
            lightOn[currentLight] = true;
          }

          // Edge-snap: touching within 12 px of an edge maps to exactly 0 or 1
          #define WHEEL_SNAP 12
          cwX = constrain((float)(touchX - WHEEL_X - WHEEL_SNAP) / (WHEEL_WIDTH  - WHEEL_SNAP * 2), 0.0f, 1.0f);
          cwY = constrain((float)(touchY - WHEEL_Y - WHEEL_SNAP) / (WHEEL_HEIGHT - WHEEL_SNAP * 2), 0.0f, 1.0f);

          currentHue = (uint8_t)(cwX * 255.0f);
          float yNorm = cwY;
          if (yNorm < 0.5f) {
            currentSat = (uint8_t)(yNorm * 2.0f * 255.0f);
            currentVal = 255;
          } else {
            currentSat = 255;
            currentVal = (uint8_t)((1.0f - yNorm) * 2.0f * 255.0f);
          }

          lightHue[currentLight] = currentHue;
          lightSat[currentLight] = currentSat;
          lightVal[currentLight] = currentVal;

          drawColorWheelCanvas();
          sendColorData();
        }
      }
      break;

    case PAGE_SETTINGS:
      {
        // 4 equal-width columns: 3 sliders + 1 mode +/- widget
        int sliderWidth  = 60;
        int modeWidth    = sliderWidth;
        int sliderHeight = 150;
        int spacing      = 10;
        int totalWidth   = (sliderWidth * 4) + (spacing * 3);
        int startX       = (320 - totalWidth) / 2;
        int centerY      = (WHEEL_Y + SNAV_Y) / 2 - 15;
        int sliderY      = centerY - sliderHeight/2;

        // Determine which column is being touched (mode column is wider)
        int column = -1;
        for (int col = 0; col < 4; col++) {
          int colX = startX + col * (sliderWidth + spacing);
          int colW = (col == 3) ? modeWidth : sliderWidth;
          if (touchX >= colX && touchX <= colX + colW) {
            column = col;
            break;
          }
        }
        
        if (column >= 0) {
          // Touch is in a valid column - map Y position within slider bounds
          int constrainedY = constrain(touchY, sliderY, sliderY + sliderHeight);
          float yPercent = 1.0 - ((float)(constrainedY - sliderY) / (float)sliderHeight);
          yPercent = constrain(yPercent, 0.0, 1.0);
          
          int oldValue = 0;
          int newValue = 0;
          
          if (column == 0) {
            // Brightness (0-255)
            oldValue = currentVal;
            newValue = (int)(yPercent * 255);
            currentVal = constrain(newValue, 0, 255);
            lightVal[currentLight] = currentVal;
          } else if (column == 1) {
            // Speed (0-255)
            oldValue = currentSpeed;
            newValue = (int)(yPercent * 255);
            currentSpeed = constrain(newValue, 0, 255);
            lightSpeed[currentLight] = currentSpeed;
          } else if (column == 2) {
            // Fadeout (0-255)
            oldValue = currentFadeout;
            newValue = (int)(yPercent * 255);
            currentFadeout = constrain(newValue, 0, 255);
            lightFadeout[currentLight] = currentFadeout;
          } else if (column == 3) {
            // Mode: top half = +, bottom half = − (no dead zone)
            oldValue = currentMode;
            int midY = sliderY + sliderHeight / 2;  // Y=120
            if (touchY < midY) {
              newValue = constrain(currentMode + 1, 0, lightMaxMode[currentLight]);
            } else {
              newValue = constrain(currentMode - 1, 0, lightMaxMode[currentLight]);
            }
            if (newValue != oldValue) lastInteractionTime = now;  // Debounce button tap
            currentMode = newValue;
            lightMode[currentLight] = currentMode;
          }
          
          // Only redraw if value actually changed
          if (newValue != oldValue) {
            sliderWasUsed = true;  // Mark that slider was used
            sendColorData();
            
            // Redraw only the active slider
            int sliderX = startX + column * (sliderWidth + spacing);
            if (column == 0) {
              drawVerticalSlider(sliderX, centerY, sliderWidth, sliderHeight, currentVal, 0, 255);
            } else if (column == 1) {
              drawVerticalSlider(sliderX, centerY, sliderWidth, sliderHeight, currentSpeed, 0, 255);
            } else if (column == 2) {
              drawVerticalSlider(sliderX, centerY, sliderWidth, sliderHeight, currentFadeout, 0, 255);
            } else if (column == 3) {
              drawModeButtons(sliderX, centerY, modeWidth, sliderHeight, currentMode);
              // Clear label area before redrawing to prevent text ghosting
              M5.Display.fillRect(sliderX, centerY + sliderHeight/2 + 8, modeWidth, 14, TFT_BLACK);
              char modeLabel[8];
              sprintf(modeLabel, "Mode %d", currentMode);
              drawSliderLabel(sliderX, centerY, modeWidth, sliderHeight, modeLabel);
            }
          }
        }
        
        wasTouching = true;
      }
      break;
  }
  
  wasTouching = true;
}

// ---------- CHECK SLEEP ----------
void checkSleep() {
  unsigned long now = millis();
  unsigned long timeSinceTouch = now - lastTouchTime;
  
  // Check for device movement (pickup detection)
  if (!displayOn || timeSinceTouch >= DISPLAY_TIMEOUT) {
    if (checkAccelerometer()) {
      // Device was picked up - just turn on backlight (no redraw needed)
      if (!displayOn) {
        Serial.println(">>> Device picked up - waking display <<<");
        M5.Display.setBrightness(128);
        displayOn = true;
        // No drawUI() call - display content is preserved!
      }
      lastTouchTime = millis(); // Reset timeout
    }
  }
  
  // Display timeout (10 seconds) - just turn off backlight
  if (displayOn && timeSinceTouch >= DISPLAY_TIMEOUT) {
    Serial.println("Display timeout - turning off backlight (pickup to wake)");
    M5.Display.setBrightness(0);
    displayOn = false;
    // Display content remains in memory, just not visible
  }
  
  // Sleep timeout (4 minutes = 240 seconds)
  if (timeSinceTouch >= SLEEP_TIMEOUT) {
    Serial.println("Entering deep sleep...");
    Serial.println("Touch screen or press power button to wake up");
    delay(100);
    
    // Wake sources:
    // GPIO 21 = Touch screen interrupt (FT6336U) — RTC GPIO, works with ext1
    // Power button wake is handled automatically by the AXP2101 PMIC via M5.Power.deepSleep()
    // NOTE: GPIO 46 is NOT an RTC GPIO on ESP32-S3, so it cannot be used for ext1 wake.
    esp_sleep_enable_ext1_wakeup((1ULL << GPIO_NUM_21), ESP_EXT1_WAKEUP_ANY_LOW);

    // Enter deep sleep via M5Unified — configures AXP2101 PMIC for power-button wake
    // After wake, device restarts from setup(); settings are preserved on SD card
    M5.Power.deepSleep(0);
  }
}

// ---------- SETUP ACCELEROMETER ----------
void setupAccelerometer() {
  // M5Unified handles IMU initialization
  // Enable IMU
  M5.Imu.begin();
  
  Serial.println("Accelerometer initialized");
  
  // Print initial accelerometer reading for debugging
  auto imu_update = M5.Imu.update();
  if (imu_update) {
    auto data = M5.Imu.getImuData();
    Serial.printf("Initial accel: X=%.2f Y=%.2f Z=%.2f\n", 
                  data.accel.x, data.accel.y, data.accel.z);
  } else {
    Serial.println("WARNING: IMU update failed!");
  }
}

// ---------- CHECK ACCELEROMETER ----------
bool checkAccelerometer() {
  static float lastX = 0, lastY = 0, lastZ = 0;
  static bool initialized = false;
  static unsigned long lastCheckTime = 0;
  unsigned long now = millis();
  
  // Check every 50ms for better responsiveness
  if (now - lastCheckTime < 50) {
    return false;
  }
  lastCheckTime = now;
  
  auto imu_update = M5.Imu.update();
  if (!imu_update) {
    return false;
  }
  
  auto data = M5.Imu.getImuData();
  
  // Initialize baseline on first run
  if (!initialized) {
    lastX = data.accel.x;
    lastY = data.accel.y;
    lastZ = data.accel.z;
    initialized = true;
    return false;
  }
  
  // Calculate change in each axis
  float deltaX = abs(data.accel.x - lastX);
  float deltaY = abs(data.accel.y - lastY);
  float deltaZ = abs(data.accel.z - lastZ);
  float maxDelta = max(deltaX, max(deltaY, deltaZ));
  
  // Update last values
  lastX = data.accel.x;
  lastY = data.accel.y;
  lastZ = data.accel.z;
  
  // Debug output
  if (maxDelta > 0.1) {  // Only print significant changes
    Serial.printf("Accel: X=%.2f Y=%.2f Z=%.2f | Delta: X=%.2f Y=%.2f Z=%.2f (max=%.2f)\n", 
                  data.accel.x, data.accel.y, data.accel.z,
                  deltaX, deltaY, deltaZ, maxDelta);
  }
  
  // Trigger on any axis change exceeding threshold
  if (maxDelta > ACCEL_THRESHOLD) {
    Serial.printf(">>> MOTION DETECTED! Delta=%.2f g (threshold=%.2f)\n", maxDelta, ACCEL_THRESHOLD);
    return true;
  }
  
  return false;
}

// ---------- HSV TO RGB CONVERSION ----------
void HSVtoRGB(uint8_t h, uint8_t s, uint8_t v, uint8_t &r, uint8_t &g, uint8_t &b) {
  if (s == 0) {
    r = g = b = v;
    return;
  }
  
  uint8_t region = h / 43;
  uint8_t remainder = (h - (region * 43)) * 6;
  
  uint8_t p = (v * (255 - s)) >> 8;
  uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
  uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
  
  switch (region) {
    case 0:
      r = v; g = t; b = p;
      break;
    case 1:
      r = q; g = v; b = p;
      break;
    case 2:
      r = p; g = v; b = t;
      break;
    case 3:
      r = p; g = q; b = v;
      break;
    case 4:
      r = t; g = p; b = v;
      break;
    default:
      r = v; g = p; b = q;
      break;
  }
}


// ---------- SHOW LOADING SCREEN ----------
void showLoadingScreen() {
  M5.Display.fillScreen(TFT_BLACK);

  // Center of screen
  int centerY = 120;  // 240 / 2
  int lineHeight = 3;
  int margin = 20;  // Empty space on each side

  // Animate line growing from left to right
  int maxWidth = 320 - (2 * margin);  // Screen width minus margins (280px)
  int startX = margin;  // Start at left margin
  int steps = 30;  // Number of animation steps

  for (int i = 0; i <= steps; i++) {
    int currentWidth = map(i, 0, steps, 0, maxWidth);

    // Draw the growing line from left to right
    M5.Display.fillRect(startX, centerY - lineHeight/2, currentWidth, lineHeight, TFT_WHITE);

    delay(30);  // Animation speed (adjust for faster/slower)
  }

  // Keep full line visible briefly before continuing
  delay(200);

}

void showShutdownAnimation() {
  // Reverse of showLoadingScreen: line shrinks from right to left, then power off.
  // Runs in a blocking loop — no input is processed during shutdown.

  int centerY   = 120;
  int lineHeight = 3;
  int margin     = 20;
  int maxWidth   = 320 - (2 * margin);   // 280 px
  int startX     = margin;
  int steps      = 30;

  // Clear UI, show full white line (mirror of end-state of startup animation)
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.fillRect(startX, centerY - lineHeight / 2, maxWidth, lineHeight, TFT_WHITE);
  delay(200);

  // Shrink line from right to left
  int prevWidth = maxWidth;
  for (int i = steps - 1; i >= 0; i--) {
    int currentWidth = map(i, 0, steps, 0, maxWidth);
    // Erase only the right-side pixels that disappear in this step
    M5.Display.fillRect(startX + currentWidth,
                        centerY - lineHeight / 2,
                        prevWidth - currentWidth,
                        lineHeight,
                        TFT_BLACK);
    prevWidth = currentWidth;
    delay(30);
  }

  delay(100);
  M5.Power.powerOff();
}

