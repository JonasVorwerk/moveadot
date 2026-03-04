# M5Stack RGB LED Remote Control System

ESP-NOW based wireless RGB LED controller using M5Stack CoreS3 as remote and ESP8266 as light receivers.

## Hardware

### Remote
- **M5Stack CoreS3** - Touch screen controller with accelerometer
- Features:
  - 320x240 IPS touchscreen
  - Built-in accelerometer for wake-on-pickup
  - SD card for settings storage
  - Built-in speaker for tactile feedback
  - Battery powered with sleep modes

### Lights (per unit)
- **ESP8266** (e.g., Wemos D1 Mini)
- **180 APA102 LEDs** (can use WS2812B with code change)
- Connections:
  - Data: GPIO 14
  - Clock: GPIO 12
  - Power: 5V supply

## Features

### Remote Control
- **3 Independent Lights** - Control up to 3 light units
- **Per-Light Settings** - Each light remembers its own:
  - Color (HSV)
  - Brightness
  - ON/OFF state
  - Animation mode
- **Flow-Based UI** - Minimalist interface with auto-return navigation
- **SD Card Storage** - All settings persist across power cycles
- **Power Management**:
  - Display timeout: 30 seconds (wake by pickup)
  - Deep sleep: 4 minutes (wake by touch or button)
- **Tactile Feedback** - Subtle click sounds on button press

### UI Pages

#### 1. Light Select (Home)
- 3 colored squares showing each light's state
- Full color = ON, Diagonal (color + black) = OFF
- Tap to select and control

#### 2. Light Main
- 3 buttons:
  - Left: ON/OFF toggle (diagonal pattern)
  - Middle: Color selector (shows current color)
  - Right: Settings (mode toggle)

#### 3. Color Selector
- Full-screen color wheel
- White → Colors → Black gradient
- Touch and drag to select
- Auto-turns light ON when color is selected

#### 4. Settings
- Mode toggle button:
  - White square = Solid color mode
  - Dotted square = Confetti animation mode

### Light Modes

#### Mode 0: Solid Color
- All LEDs display the selected color
- Instant color changes

#### Mode 1: Confetti
- Random colored sparkles that fade out
- Uses selected hue as base color with variations
- 30 FPS smooth animation

## Communication Protocol

### ESP-NOW Message Structure
```cpp
struct {
  uint8_t h;      // Hue (0-255)
  uint8_t s;      // Saturation (0-255)
  uint8_t v;      // Value/Brightness (0-255)
  bool power;     // ON/OFF state
  int mode;       // Animation mode (0=solid, 1=confetti)
}
```

### MAC Address Configuration
Stored on SD card in `/remote_settings.txt`:
```
light0_mac=60:01:94:4C:0C:57
light1_mac=5C:CF:7F:C6:D6:1E
light2_mac=60:01:94:4C:16:A8
```

## SD Card Settings File

Located at `/remote_settings.txt`, contains:
```
currentLight=0
light0_hue=30
light0_sat=200
light0_val=180
light0_on=1
light0_mode=0
light0_mac=60:01:94:4C:0C:57
light1_hue=170
light1_sat=255
light1_val=200
light1_on=0
light1_mode=1
light1_mac=5C:CF:7F:C6:D6:1E
light2_hue=20
light2_sat=150
light2_val=180
light2_on=1
light2_mode=0
light2_mac=60:01:94:4C:16:A8
```

## Installation

### Remote (M5Stack CoreS3)
1. Install Arduino IDE with ESP32 support
2. Install libraries:
   - M5Unified
   - SD
   - WiFi (ESP32 core)
   - esp_now (ESP32 core)
3. Upload `remote.ino`
4. Insert SD card (formatted FAT32)
5. Configure MAC addresses on first boot or via SD card

### Light (ESP8266)
1. Install Arduino IDE with ESP8266 support
2. Install libraries:
   - ESP8266WiFi
   - espnow
   - FastLED
3. Update LED configuration in code if needed:
   ```cpp
   #define NUM_LEDS 180
   FastLED.addLeds<APA102, 14, 12, BGR>(leds, NUM_LEDS);
   // or for WS2812B:
   // FastLED.addLeds<WS2812B, 14, RGB>(leds, NUM_LEDS);
   ```
4. Upload `light.ino`
5. Note MAC address from Serial Monitor
6. Add MAC to remote's SD card settings

## Usage Tips

- **Battery Indicator**: Small dot in top-right corner
  - Green: >60%
  - Orange: 30-60%
  - Red: <30%

- **Navigation**: 
  - Pages auto-return after 3 seconds of inactivity
  - Color wheel allows continuous dragging without debounce
  - All buttons have 600ms debounce to prevent accidental triggers

- **First Setup**:
  1. Upload code to all devices
  2. Note MAC addresses from Serial Monitor
  3. Create or edit `/remote_settings.txt` on SD card
  4. Restart remote - settings will load automatically

- **Troubleshooting**:
  - If lights don't respond: verify MAC addresses match
  - If settings don't save: check SD card is formatted FAT32
  - If sleep doesn't wake: touch screen or press power button

## Code Architecture

### Remote
- **Flow-based navigation** - Pages transition with auto-return
- **Single debounce timer** - 600ms for all button interactions
- **State management** - Per-light arrays + current active light
- **Settings auto-save** - Debounced SD writes (2s cooldown)
- **Power modes** - Display dim → Deep sleep with multiple wake sources

### Light
- **Event-driven** - Updates only on ESP-NOW message receive
- **FastLED library** - Hardware-accelerated LED control
- **Mode system** - Extensible animation framework
- **FPS limiting** - Consistent 30 FPS with FastLED.delay()

## Version History

**v2.0 (Current)** - Experimental Flow UI
- Complete UI redesign
- Per-light ON/OFF state (separate from brightness)
- Animation mode support
- Tactile audio feedback
- Enhanced power management

**v1.0** - Multi-page UI (deprecated)
- Three-page interface (Color, Brightness, Presets)
- Light bar with tap-to-switch
- Swipe navigation

## License

MIT License - Free to use and modify

## Contributing

This is a personal project, but suggestions and improvements are welcome!
