/*
 * RGB LED Remote Control - M5Stack CoreS3/SE + Core2
 *
 * Features:
 * - 2D HSV color wheel (like traditional color picker)
 * - Hue: angle around the circle
 * - Brightness/Lightness: radial distance (center=white/bright, edge=dark/vivid)
 * - Saturation: radial distance (center=desaturated/white, edge=saturated)
 * - Display timeout after 20 seconds with pickup wake
 * - Power off via power button (stays on when charging)
 * - ESP-NOW transmission to Light device
 * - Runtime board detection (CoreS3/SE vs Core2)
 */

#include <M5Unified.h>
#include <esp_now.h>
#include <WiFi.h>
// Maximum number of discoverable lights (runtime count stored in numLights)
#define MAX_LIGHTS 8


// Broadcast MAC for ESP-NOW discovery
static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

uint8_t lightMacAddresses[MAX_LIGHTS][6];  // Filled at runtime during discovery

// Display settings
#define DISPLAY_TIMEOUT 20000   // 20 seconds before display dims
#define POWER_OFF_TIMEOUT 60000  // 1 minute before auto power off (when not charging)

// Light color bar at top
#define LIGHT_BAR_HEIGHT 25         // Increased (was 20)
#define LIGHT_BAR_Y 0
#define LIGHT_BAR_INDICATOR_HEIGHT 3  // Active light indicator thickness
#define LIGHT_BAR_GAP 3  // Gap between light bar and content below

// Page system - new flow
#define NUM_PAGES 8
#define PAGE_LOADING        0
#define PAGE_LIGHT_SELECT   1
#define PAGE_LIGHT_MAIN     2
#define PAGE_PRESETS        3   // Settings group: first sub-page
#define PAGE_HSV            4
#define PAGE_COLOR_SELECTOR 5
#define PAGE_SETTINGS       6
#define PAGE_MODE           7
#define IS_SETTINGS_PAGE(p) ((p) >= PAGE_PRESETS && (p) <= PAGE_MODE)

// Auto-return timeout
#define AUTO_RETURN_TIMEOUT 5000  // 3 seconds

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
#define BATTERY_INDICATOR_RADIUS 4  // 4px radius = 8px diameter

// Button dimensions for new UI
#define BUTTON_MARGIN 20
#define BUTTON_HEIGHT 60
#define BUTTON_SPACING 10

// UI font and size //https://m5stack.lang-ship.com/howto/m5gfx/font/#google_vignette 
#define UI_FONT      Font2 //FreeSans9pt7b

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
uint8_t lastSettingsPage = PAGE_SETTINGS;  // Last visited settings sub-page
const char* settingsPageNames[] = { "Presets", "HSV", "Color", "Settings", "Mode" };
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
  bool requestState;      // If true: light should reply with its state, not apply changes
  bool isDiscovery;       // If true: discovery broadcast — light should reply with its name
  char deviceName[16];    // Device name (sent in discovery reply)
  int  maxMode;           // Max mode index, sent by lamp during discovery
  bool requestPresets;    // If true: light should reply with its preset list
  bool requestModeNames;  // If true: light should reply with mode names list
} struct_message;

// ---------- MODE NAMES PACKET ----------
// One packet per mode — fixture can have any number of modes without reflashing remote
#define MAX_MODE_NAME_LEN 16
#define MAX_MODES_REMOTE  30   // Max modes any fixture can ever have
typedef struct {
  bool    isModeNamePacket;
  uint8_t modeIndex;
  uint8_t totalModes;
  char    name[MAX_MODE_NAME_LEN];
} mode_name_packet;

// Per-light mode name storage
char    lightModeNames[MAX_LIGHTS][MAX_MODES_REMOTE][MAX_MODE_NAME_LEN];
uint8_t lightModeNamesReceived[MAX_LIGHTS];
bool    lightModeNamesLoaded[MAX_LIGHTS];

struct_message outgoingData;

// Runtime device count (filled during discovery)
int numLights = 0;

// ESP-NOW peer info
esp_now_peer_info_t peerInfo;

// State-read tracking (set by receive callback during requestLightStates)
bool lightStateReceived[MAX_LIGHTS];

// ---------- FUNCTION DECLARATIONS ----------
void setupESPNow();
void requestLightStates();
void requestLampPresets(int lightIdx);
void requestLampModeNames(int lightIdx);
void sendColorData();
void buildColorWheel();
void drawColorWheelCanvas();
void syncCwFromColor();
void drawUI();
void drawPresetsPage();
void drawSettingsNav();
void handleSettingsNavTap(int tx, int ty);
void drawLightBar();
void drawBatteryIndicator();
void drawLightSelectPage();
void drawLightMainPage();
void drawColorSelectorPage();
void drawSettingsPage();
void drawHSVPage();
void drawModePage();
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
void playClick();

// ---------- BOARD DETECTION ----------
// Detected once in setup(); used to branch hardware-specific code.
static bool isCore2 = false;

// ---------- POWER KEY ----------
// CoreS3/SE: M5.BtnPWR is broken, must read AXP2101 directly via I2C.
//   Register 0x49 bit 1 = short press. Must read BEFORE M5.update().
// Core2: M5.BtnPWR works via M5Unified — read AFTER M5.update().

static inline uint8_t axpRead(uint8_t reg) {
  return M5.In_I2C.readRegister8(0x34, reg, 400000UL);
}
static inline void axpWrite(uint8_t reg, uint8_t val) {
  M5.In_I2C.writeRegister8(0x34, reg, val, 400000UL);
}

static uint32_t powerKeyReadyAt = 0;

void initPowerKey() {
  if (isCore2) {
    // M5Unified handles AXP192 / BtnPWR natively on Core2
    powerKeyReadyAt = millis() + 4000;
    return;
  }
  // CoreS3/SE — AXP2101 direct I2C
  axpWrite(0x41, axpRead(0x41) | 0x03);  // enable short-press + release IRQs
  axpWrite(0x25, 0x00);                  // minimum short-press timer
  axpWrite(0x49, 0x03);                  // clear stale boot-press bits
  powerKeyReadyAt = millis() + 4000;
}

bool powerKeyShortPressed() {
  if (millis() < powerKeyReadyAt) return false;
  if (isCore2) {
    return M5.BtnPWR.wasClicked();
  }
  // CoreS3/SE — AXP2101 direct I2C
  uint8_t irq1 = axpRead(0x49);
  if (irq1 & 0x03) axpWrite(0x49, irq1 & 0x03);  // clear bits
  return (irq1 & 0x02) != 0;
}

// ---------- SETUP ----------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Display.setRotation(1);
  M5.Display.setBrightness(128);

  // Detect board type (must be after M5.begin)
  isCore2 = (M5.getBoard() == m5::board_t::board_M5StackCore2);

  Serial.begin(115200);
  delay(100);
  Serial.printf("RGB Remote Starting... Board: %s\n", isCore2 ? "Core2" : "CoreS3/SE");

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
  unsigned long discoveryDeadline = millis() + 500;
  Serial.println("Discovery broadcast sent");

  // Show loading animation (~1.1 s). ESP-NOW callbacks run in a
  // FreeRTOS Wi-Fi task so discovery replies are collected in parallel.
  showLoadingScreen();

  // Wait out any remaining discovery time (usually only a few hundred ms)
  while (millis() < discoveryDeadline) delay(20);
  Serial.printf("Discovery complete: %d device(s) found\n", numLights);

  // Read current state and mode names from discovered lights
  if (numLights > 0) {
    requestLightStates();
    for (int i = 0; i < numLights; i++) requestLampModeNames(i);
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
  // CoreS3/SE: check power button BEFORE M5.update() so M5Unified can't clear AXP2101 IRQ bits.
  // Core2: M5.BtnPWR is updated by M5.update(), so it must be checked after.
  if (!isCore2 && powerKeyShortPressed()) {
    if (!M5.Power.isCharging()) {
      Serial.println("Power button — shutting down");
      showShutdownAnimation();
    } else {
      Serial.println("Power button — ignored (charging)");
    }
  }

  M5.update();

  // Core2: power button check after M5.update()
  if (isCore2 && powerKeyShortPressed()) {
    if (!M5.Power.isCharging()) {
      Serial.println("Power button — shutting down");
      showShutdownAnimation();
    } else {
      Serial.println("Power button — ignored (charging)");
    }
  }

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
    } else if ((currentPage == PAGE_COLOR_SELECTOR || currentPage == PAGE_SETTINGS || currentPage == PAGE_HSV) && !ignoreFirstTouch) {
      // Color selector and settings sliders need continuous drag updates
      handleTouch();
    }
    lastTouchTime = millis();
  }
  
  // Detect touch release
  if (wasTouching && !isTouching) {
    // Play click sound on release only if slider was actually dragged
    if ((currentPage == PAGE_SETTINGS || currentPage == PAGE_HSV) && sliderWasUsed) {
      playClick();
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

  // ── Single mode name packet ─────────────────────────────────────────────
  if (len == (int)sizeof(mode_name_packet)) {
    const mode_name_packet *mn = (mode_name_packet*)data;
    if (mn->isModeNamePacket && mn->modeIndex < MAX_MODES_REMOTE) {
      for (int i = 0; i < numLights; i++) {
        if (memcmp(mac, lightMacAddresses[i], 6) == 0) {
          strncpy(lightModeNames[i][mn->modeIndex], mn->name, MAX_MODE_NAME_LEN - 1);
          lightModeNames[i][mn->modeIndex][MAX_MODE_NAME_LEN - 1] = '\0';
          lightModeNamesReceived[i]++;
          if (lightModeNamesReceived[i] >= mn->totalModes) {
            lightModeNamesLoaded[i] = true;
            Serial.printf("All mode names received from Light %d (%s)\n", i + 1, lightNames[i]);
          }
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
    lightMaxMode[idx] = msg->maxMode;

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
      Serial.printf("State received from Light %d (%s): H=%d S=%d V=%d ON=%d Mode=%d Speed=%d Fade=%d\n",
                    i + 1, lightNames[i], msg->h, msg->s, msg->v, msg->power, msg->mode, msg->speed, msg->fadeout);
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

  // Wait up to 500ms for all lights to respond
  unsigned long deadline = millis() + 500;
  while (millis() < deadline) {
    bool allDone = true;
    for (int i = 0; i < numLights; i++) {
      if (!lightStateReceived[i]) { allDone = false; break; }
    }
    if (allDone) break;
    delay(20);
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

// ---------- REQUEST MODE NAMES ----------
void requestLampModeNames(int idx) {
  if (idx < 0 || idx >= numLights) return;
  Serial.printf("Requesting mode names from Light %d (%s)...\n", idx + 1, lightNames[idx]);

  lightModeNamesReceived[idx] = 0;
  lightModeNamesLoaded[idx]   = false;

  struct_message req = {};
  req.requestModeNames = true;
  esp_err_t result = esp_now_send(lightMacAddresses[idx], (uint8_t*)&req, sizeof(req));
  if (result != ESP_OK) {
    Serial.printf("Mode names request send failed for Light %d\n", idx + 1);
    return;
  }

  // Wait up to 3s — modes × 20ms per packet + loop() frame time buffer
  unsigned long deadline = millis() + 3000;
  while (millis() < deadline && !lightModeNamesLoaded[idx]) delay(20);

  if (lightModeNamesLoaded[idx]) {
    Serial.printf("Mode names loaded for Light %d\n", idx + 1);
  } else {
    Serial.printf("Mode names timeout for Light %d (%d/%d received)\n",
                  idx + 1, lightModeNamesReceived[idx], lightMaxMode[idx] + 1);
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
    Serial.printf("Sent to Light %d: H=%d S=%d V=%d Power=%d Mode=%d Speed=%d Fade=%d\n", currentLight + 1, currentHue, currentSat, currentVal, currentOn, currentMode, currentSpeed, currentFadeout);
  } else {
    Serial.printf("Error sending to Light %d\n", currentLight + 1);
  }
}

// ---------- DRAW UI ----------
// ---------- DRAW PRESETS PAGE ----------
void drawPresetsPage() {
  // Load presets from lamp if not yet fetched
  if (!lightPresetsLoaded[currentLight]) {
    M5.Display.setFont(&UI_FONT);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextSize(1);
    M5.Display.drawString("LOADING...", 160, 100);
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

    M5.Display.setFont(&UI_FONT);
    M5.Display.setTextColor(M5.Display.color565(100, 100, 100), TFT_BLACK);
    M5.Display.setTextDatum(top_center);
    M5.Display.setTextSize(1);
    const char* label = (i < count) ? lightPresets[currentLight][i].name : "";
    M5.Display.drawString(upper(label), x + sqW / 2, y + sqW + 6);
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
  M5.Display.setFont(&UI_FONT);
  M5.Display.setTextColor(SNAV_COL_ACT, TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(1);
  M5.Display.drawString(upper(name), SNAV_TITLE_X + SNAV_TITLE_W / 2, cy);

  M5.Display.endWrite();
}

// ---------- HANDLE SETTINGS NAV TAP ----------
void handleSettingsNavTap(int tx, int ty) {
  if (tx < SNAV_BTN_W) {
    // Left — previous, wrap around
    uint8_t next = (currentPage <= PAGE_PRESETS) ? PAGE_MODE : currentPage - 1;
    navigateToPage(next, true);
  } else if (tx >= 320 - SNAV_BTN_W) {
    // Right — next, wrap around
    uint8_t next = (currentPage >= PAGE_MODE) ? PAGE_PRESETS : currentPage + 1;
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
    case PAGE_HSV:            drawHSVPage();            break;
    case PAGE_COLOR_SELECTOR: drawColorSelectorPage();  break;
    case PAGE_SETTINGS:       drawSettingsPage();       break;
    case PAGE_MODE:           drawModePage();           break;
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
  bool isCharging = M5.Power.isCharging();
  bool isUsbConnected = isCharging || (!isCore2 && M5.Power.Axp2101.isVBUS());

  // Determine color based on battery and power state
  uint16_t batteryColor;
  if (isUsbConnected) {
    // Blue when USB connected (charging or full on USB)
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
  
  // Clear the indicator area (text + dot) before redrawing to avoid ghosting
  M5.Display.fillRect(270, 0, 50, 14, TFT_BLACK);

  // Draw percentage text just left of the dot
  char buf[6];
  snprintf(buf, sizeof(buf), "%d%%", batteryLevel);
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextSize(1.0f);
  M5.Display.setTextDatum(middle_right);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.drawString(buf, BATTERY_INDICATOR_X - BATTERY_INDICATOR_RADIUS - 3, BATTERY_INDICATOR_Y + 1);

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

  Serial.printf("Switched to Light %d (%s) H=%d S=%d V=%d ON=%d Mode=%d Speed=%d Fade=%d\n",
                currentLight + 1, lightNames[currentLight],
                currentHue, currentSat, currentVal, currentOn, currentMode, currentSpeed, currentFadeout);

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
    M5.Display.setFont(&UI_FONT);
    M5.Display.setTextSize(1);
    M5.Display.setTextDatum(middle_center);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.drawString("NO DEVICES", 160, 100);
    M5.Display.drawString("FOUND", 160, 120);
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
    M5.Display.setFont(&UI_FONT);
    M5.Display.setTextColor(M5.Display.color565(100, 100, 100), TFT_BLACK);
    M5.Display.setTextDatum(top_center);
    M5.Display.setTextSize(1);
    M5.Display.drawString(upper(lightNames[i]), x + squareSize / 2, y + squareSize + 6);
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
    M5.Display.setFont(&UI_FONT);
    M5.Display.setTextColor(M5.Display.color565(100, 100, 100));  // Dark grey
    M5.Display.setTextDatum(top_center);
    M5.Display.setTextSize(1);
    M5.Display.drawString(upper(labels[i]), x + squareSize/2, y + squareSize + 6);
  }
}

// ---------- DRAW COLOR SELECTOR PAGE ----------
void drawColorSelectorPage() {
  syncCwFromColor();
  drawColorWheelCanvas();
}

// ---------- DRAW HSV PAGE ----------
void drawHSVPage() {
  int sliderWidth  = 80;
  int sliderHeight = 150;
  int startX       = (320 - (sliderWidth * 3 + BUTTON_SPACING * 2)) / 2;
  int centerY      = (WHEEL_Y + SNAV_Y) / 2 - 15;

  // Hue (0-255)
  drawVerticalSlider(startX, centerY, sliderWidth, sliderHeight, currentHue, 0, 255);
  drawSliderLabel(startX, centerY, sliderWidth, sliderHeight, "Hue");

  // Saturation (0-255)
  drawVerticalSlider(startX + (sliderWidth + BUTTON_SPACING), centerY, sliderWidth, sliderHeight, currentSat, 0, 255);
  drawSliderLabel(startX + (sliderWidth + BUTTON_SPACING), centerY, sliderWidth, sliderHeight, "Sat");

  // Brightness (0-255)
  drawVerticalSlider(startX + (sliderWidth + BUTTON_SPACING) * 2, centerY, sliderWidth, sliderHeight, currentVal, 0, 255);
  drawSliderLabel(startX + (sliderWidth + BUTTON_SPACING) * 2, centerY, sliderWidth, sliderHeight, "Bright");
}

// ---------- DRAW MODE PAGE ----------
void drawModePage() {
  int squareSize = 80;
  int totalWidth = (squareSize * 3) + (BUTTON_SPACING * 2);
  int startX     = (320 - totalWidth) / 2;
  int y          = (240 - squareSize) / 2;
  int leftX      = startX;
  int midX       = startX + squareSize + BUTTON_SPACING;
  int rightX     = startX + (squareSize + BUTTON_SPACING) * 2;

  // Clear content area
  M5.Display.fillRect(0, WHEEL_Y, 320, SNAV_Y - WHEEL_Y, TFT_BLACK);

  // Mode name centered above the buttons
  char modeName[MAX_MODE_NAME_LEN + 1];
  if (lightModeNamesLoaded[currentLight] && currentMode < MAX_MODES_REMOTE) {
    strncpy(modeName, lightModeNames[currentLight][currentMode], sizeof(modeName) - 1);
    modeName[sizeof(modeName) - 1] = '\0';
  } else {
    sprintf(modeName, "Mode %d", currentMode);
  }
  M5.Display.setFont(&fonts::Font4);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(1);
  M5.Display.drawString(upper(modeName), 160, y - 31);

  // Prev button
  M5.Display.fillRect(leftX, y, squareSize, squareSize, TFT_WHITE);
  M5.Display.setFont(&UI_FONT);
  M5.Display.setTextColor(M5.Display.color565(100, 100, 100));
  M5.Display.setTextDatum(top_center);
  M5.Display.setTextSize(1);
  M5.Display.drawString("PREV", leftX + squareSize / 2, y + squareSize + 6);

  // Random button (middle)
  M5.Display.fillRect(midX, y, squareSize, squareSize, TFT_WHITE);
  M5.Display.setFont(&UI_FONT);
  M5.Display.setTextColor(M5.Display.color565(100, 100, 100));
  M5.Display.setTextDatum(top_center);
  M5.Display.setTextSize(1);
  M5.Display.drawString("RANDOM", midX + squareSize / 2, y + squareSize + 6);

  // Next button
  M5.Display.fillRect(rightX, y, squareSize, squareSize, TFT_WHITE);
  M5.Display.setFont(&UI_FONT);
  M5.Display.setTextColor(M5.Display.color565(100, 100, 100));
  M5.Display.setTextDatum(top_center);
  M5.Display.setTextSize(1);
  M5.Display.drawString("NEXT", rightX + squareSize / 2, y + squareSize + 6);
}

// ---------- DRAW SETTINGS PAGE ----------
void drawSettingsPage() {
  // 3 vertical sliders: Brightness, Speed, Fade — aligned with main page buttons
  int sliderWidth  = 80;
  int sliderHeight = 150;
  int startX       = (320 - (sliderWidth * 3 + BUTTON_SPACING * 2)) / 2;
  int centerY      = (WHEEL_Y + SNAV_Y) / 2 - 15;

  // Brightness (0-255)
  drawVerticalSlider(startX, centerY, sliderWidth, sliderHeight, currentVal, 0, 255);
  drawSliderLabel(startX, centerY, sliderWidth, sliderHeight, "Bright");

  // Speed (0-255)
  drawVerticalSlider(startX + (sliderWidth + BUTTON_SPACING), centerY, sliderWidth, sliderHeight, currentSpeed, 0, 255);
  drawSliderLabel(startX + (sliderWidth + BUTTON_SPACING), centerY, sliderWidth, sliderHeight, "Speed");

  // Fadeout (0-255)
  drawVerticalSlider(startX + (sliderWidth + BUTTON_SPACING) * 2, centerY, sliderWidth, sliderHeight, currentFadeout, 0, 255);
  drawSliderLabel(startX + (sliderWidth + BUTTON_SPACING) * 2, centerY, sliderWidth, sliderHeight, "Fade");
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
  int y = centerY + height/2 + 6;  // 6px below slider

  M5.Display.setFont(&UI_FONT);  // Small sans-serif font
  M5.Display.setTextColor(M5.Display.color565(100, 100, 100));  // Dark grey
  M5.Display.setTextDatum(top_center);
  M5.Display.setTextSize(1);
  M5.Display.drawString(upper(label), x + width/2, y);
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
    playClick();  // 4kHz for 20ms - subtle click
  }
  
  previousPage = currentPage;
  currentPage = page;
  if (IS_SETTINGS_PAGE(page)) lastSettingsPage = page;
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
              playClick();  // Click sound
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
              navigateToPage(lastSettingsPage, true);  // Return to last visited settings sub-page
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
            playClick();
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

    case PAGE_HSV:
      {
        int sliderWidth  = 80;
        int sliderHeight = 150;
        int startX       = (320 - (sliderWidth * 3 + BUTTON_SPACING * 2)) / 2;
        int centerY      = (WHEEL_Y + SNAV_Y) / 2 - 15;
        int sliderY      = centerY - sliderHeight / 2;

        int column = -1;
        for (int col = 0; col < 3; col++) {
          int colX = startX + col * (sliderWidth + BUTTON_SPACING);
          if (touchX >= colX && touchX <= colX + sliderWidth) {
            column = col;
            break;
          }
        }

        if (column >= 0) {
          int constrainedY = constrain(touchY, sliderY, sliderY + sliderHeight);
          float yPercent = 1.0 - ((float)(constrainedY - sliderY) / (float)sliderHeight);
          yPercent = constrain(yPercent, 0.0, 1.0);

          int newValue = (int)(yPercent * 255);
          int oldValue = 0;

          if (column == 0) {
            oldValue = currentHue;
            currentHue = constrain(newValue, 0, 255);
            lightHue[currentLight] = currentHue;
          } else if (column == 1) {
            oldValue = currentSat;
            currentSat = constrain(newValue, 0, 255);
            lightSat[currentLight] = currentSat;
          } else if (column == 2) {
            oldValue = currentVal;
            currentVal = constrain(newValue, 0, 255);
            lightVal[currentLight] = currentVal;
          }

          if (newValue != oldValue) {
            sliderWasUsed = true;
            sendColorData();
            int sliderX = startX + column * (sliderWidth + BUTTON_SPACING);
            uint8_t drawVal = (column == 0) ? currentHue : (column == 1) ? currentSat : currentVal;
            drawVerticalSlider(sliderX, centerY, sliderWidth, sliderHeight, drawVal, 0, 255);
          }
        }

        wasTouching = true;
      }
      break;

    case PAGE_SETTINGS:
      {
        // 3 sliders: Brightness, Speed, Fade
        int sliderWidth  = 80;
        int sliderHeight = 150;
        int startX       = (320 - (sliderWidth * 3 + BUTTON_SPACING * 2)) / 2;
        int centerY      = (WHEEL_Y + SNAV_Y) / 2 - 15;
        int sliderY      = centerY - sliderHeight / 2;

        int column = -1;
        for (int col = 0; col < 3; col++) {
          int colX = startX + col * (sliderWidth + BUTTON_SPACING);
          if (touchX >= colX && touchX <= colX + sliderWidth) { column = col; break; }
        }

        if (column >= 0) {
          int constrainedY = constrain(touchY, sliderY, sliderY + sliderHeight);
          float yPercent   = 1.0f - ((float)(constrainedY - sliderY) / (float)sliderHeight);
          yPercent = constrain(yPercent, 0.0f, 1.0f);
          int newValue = (int)(yPercent * 255);
          int oldValue = 0;

          if (column == 0) {
            oldValue = currentVal;    currentVal    = constrain(newValue, 0, 255); lightVal[currentLight]    = currentVal;
          } else if (column == 1) {
            oldValue = currentSpeed;  currentSpeed  = constrain(newValue, 0, 255); lightSpeed[currentLight]  = currentSpeed;
          } else if (column == 2) {
            oldValue = currentFadeout; currentFadeout = constrain(newValue, 0, 255); lightFadeout[currentLight] = currentFadeout;
          }

          if (newValue != oldValue) {
            sliderWasUsed = true;
            sendColorData();
            int sliderX = startX + column * (sliderWidth + BUTTON_SPACING);
            uint8_t drawVal = (column == 0) ? currentVal : (column == 1) ? currentSpeed : currentFadeout;
            drawVerticalSlider(sliderX, centerY, sliderWidth, sliderHeight, drawVal, 0, 255);
          }
        }

        wasTouching = true;
      }
      break;

    case PAGE_MODE:
      {
        int squareSize = 80;
        int totalWidth = (squareSize * 3) + (BUTTON_SPACING * 2);
        int startX     = (320 - totalWidth) / 2;
        int leftX      = startX;
        int midX       = startX + squareSize + BUTTON_SPACING;
        int rightX     = startX + (squareSize + BUTTON_SPACING) * 2;
        int btnY       = (240 - squareSize) / 2;

        if (!wasTouching) {
          int oldMode = currentMode;
          bool changed = false;

          if (touchX >= leftX && touchX <= leftX + squareSize &&
              touchY >= btnY  && touchY <= btnY + squareSize) {
            currentMode = constrain(currentMode - 1, 0, lightMaxMode[currentLight]);
            changed = (currentMode != oldMode);
          } else if (touchX >= rightX && touchX <= rightX + squareSize &&
                     touchY >= btnY   && touchY <= btnY + squareSize) {
            currentMode = constrain(currentMode + 1, 0, lightMaxMode[currentLight]);
            changed = (currentMode != oldMode);
          } else if (touchX >= midX && touchX <= midX + squareSize &&
                     touchY >= btnY  && touchY <= btnY + squareSize) {
            currentHue    = random(256);
            currentSat    = random(100, 256);
            currentVal    = random(80, 256);
            currentSpeed  = random(256);
            currentFadeout = random(256);
            lightHue[currentLight]    = currentHue;
            lightSat[currentLight]    = currentSat;
            lightVal[currentLight]    = currentVal;
            lightSpeed[currentLight]  = currentSpeed;
            lightFadeout[currentLight] = currentFadeout;
            changed = true;
          }

          if (changed) {
            lightMode[currentLight] = currentMode;
            sendColorData();
            playClick();
            drawModePage();
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
  
  // Display timeout - just turn off backlight
  if (displayOn && timeSinceTouch >= DISPLAY_TIMEOUT) {
    Serial.println("Display timeout - turning off backlight (pickup to wake)");
    M5.Display.setBrightness(0);
    displayOn = false;
  }

  // Auto power off after 2 minutes of inactivity (only when not charging)
  bool usbPresent = M5.Power.isCharging() || (!isCore2 && M5.Power.Axp2101.isVBUS());
  if (timeSinceTouch >= POWER_OFF_TIMEOUT && !usbPresent) {
    Serial.println("Auto power off — no activity and not charging");
    showShutdownAnimation();
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

// ---------- UPPERCASE HELPER ----------
// Converts string to uppercase in a shared buffer — use immediately, don't store.
static char _upperBuf[32];
const char* upper(const char* s) {
  int i = 0;
  for (; s[i] && i < (int)sizeof(_upperBuf) - 1; i++)
    _upperBuf[i] = toupper((unsigned char)s[i]);
  _upperBuf[i] = '\0';
  return _upperBuf;
}

// ---------- CLICK FEEDBACK ----------
void playClick() {

  // if (isCore2) {
  //   M5.Power.setVibration(255); delay(75);
  //   M5.Power.setVibration(0);   delay(10);
  // } else {
     M5.Speaker.tone(4000, 10);
  // }

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
  int steps = 20;  // Number of animation steps

  for (int i = 0; i <= steps; i++) {
    int currentWidth = map(i, 0, steps, 0, maxWidth);

    // Draw the growing line from left to right
    M5.Display.fillRect(startX, centerY - lineHeight/2, currentWidth, lineHeight, TFT_WHITE);

    delay(15);  // Animation speed
  }

  // Keep full line visible briefly before continuing
  delay(100);

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

