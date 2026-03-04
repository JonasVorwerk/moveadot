/*
 * UX.ino  –  Page layout experiments  (M5Stack CoreS3/SE  320 × 240)
 *
 * Page 0:  Light selector  (2 × 2, full screen, no nav bar)
 * Page 1:  6 buttons  (3 × 2 grid)
 * Page 2:  M5Canvas colour preview — R/G/B horizontal sliders
 * Page 3:  4 vertical sliders  (M5Canvas)
 * Page 4:  HSV colour wheel  (M5Canvas, pre-rendered background)
 *
 * Navigation: dedicated nav bar at the bottom
 *   ◀ (60 px) | page name (200 px) | ▶ (60 px)
 *
 * Speed: startWrite/endWrite batching + M5Canvas DMA push
 *   Colour wheel: pre-rendered once into wheelBg sprite,
 *   blit wheelBg → canvas, draw cursor, pushSprite (zero tearing)
 */

#include <M5Unified.h>

// ── Screen ────────────────────────────────────────────────────────────────
#define SCR_W     320
#define SCR_H     240

// ── Layout ────────────────────────────────────────────────────────────────
#define NAV_H     36
#define CONTENT_H (SCR_H - NAV_H)   // 204 px
#define GAP       4

// Nav bar geometry
#define NAV_Y       CONTENT_H
#define NAV_BTN_W   60
#define NAV_TITLE_X NAV_BTN_W
#define NAV_TITLE_W (SCR_W - NAV_BTN_W * 2)

// Nav colours — no background, all elements in dark grey
#define NAV_BTN_ACT   0x630C   // dark grey  (~#606060)  active arrow / title
#define NAV_BTN_DIM   0x2945   // very dark grey          dimmed arrow

// ── Pages ─────────────────────────────────────────────────────────────────
#define NUM_PAGES       5
#define PAGE_4BTN       0
#define PAGE_6BTN       1
#define PAGE_CANVAS     2
#define PAGE_VSLDR4     3
#define PAGE_COLORWHEEL 4

const char* pageNames[NUM_PAGES] = {
  "4 Buttons", "6 Buttons", "Canvas", "V Sliders", "Color Wheel"
};

uint8_t currentPage = PAGE_4BTN;

// ── Slider state ──────────────────────────────────────────────────────────
float vSlider[3]  = { 0.5f, 0.5f, 0.5f };        // canvas page  (R/G/B)
float vs4[4]      = { 0.5f, 0.5f, 0.5f, 0.5f };  // vertical slider page

// ── Colour wheel state ────────────────────────────────────────────────────
float cwX       = 0.5f;   // normalised x  →  hue
float cwY       = 0.25f;  // normalised y  →  sat / val position
bool  wheelReady = false;

// ── M5Canvas sprites ──────────────────────────────────────────────────────
M5Canvas canvas(&M5.Lcd);   // working sprite  (320 × CONTENT_H)
M5Canvas wheelBg(&M5.Lcd);  // pre-rendered HSV gradient  (320 × CONTENT_H)

// ── Touch tracking ────────────────────────────────────────────────────────
bool wasTouching  = false;
bool isDragging   = false;
bool sliderActive = false;
int  touchStartX  = 0, touchStartY = 0;
int  lockedSlider = -1;

#define DRAG_THRESH 8
#define BAR_SNAP    12   // px from edge → snaps to 0 % or 100 %

// ── Rect helper ───────────────────────────────────────────────────────────
struct Rect { int x, y, w, h; };
bool inRect(int tx, int ty, Rect r) {
  return tx >= r.x && tx < r.x + r.w &&
         ty >= r.y && ty < r.y + r.h;
}

void drawCurrentPage();

// ── Value helpers (edge snap) ─────────────────────────────────────────────
static inline float hBarVal(int tx) {
  return constrain((float)(tx - BAR_SNAP) / (SCR_W - BAR_SNAP * 2), 0.0f, 1.0f);
}
static inline float vBarVal(int ty, int top, int h) {
  return constrain(1.0f - (float)(ty - top - BAR_SNAP) / (h - BAR_SNAP * 2), 0.0f, 1.0f);
}

// ── HSV → RGB ─────────────────────────────────────────────────────────────
void HSVtoRGB(uint8_t h, uint8_t s, uint8_t v,
              uint8_t &r, uint8_t &g, uint8_t &b) {
  if (s == 0) { r = g = b = v; return; }
  uint8_t region    = h / 43;
  uint8_t remainder = (h - region * 43) * 6;
  uint8_t p = (v * (255 - s)) >> 8;
  uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
  uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
  switch (region) {
    case 0: r=v; g=t; b=p; break;
    case 1: r=q; g=v; b=p; break;
    case 2: r=p; g=v; b=t; break;
    case 3: r=p; g=q; b=v; break;
    case 4: r=t; g=p; b=v; break;
    default: r=v; g=p; b=q; break;
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  NAV BAR
// ═══════════════════════════════════════════════════════════════════════════

void drawNav() {
  M5.Lcd.startWrite();
  M5.Lcd.fillRect(0, NAV_Y, SCR_W, NAV_H, TFT_BLACK);

  int cy = NAV_Y + NAV_H / 2;
  // Both arrows always active — circular navigation
  uint16_t lCol = NAV_BTN_ACT;
  uint16_t rCol = NAV_BTN_ACT;

  M5.Lcd.fillTriangle(
    NAV_BTN_W / 2 - 6, cy,
    NAV_BTN_W / 2 + 6, cy - 7,
    NAV_BTN_W / 2 + 6, cy + 7,
    lCol);

  M5.Lcd.fillTriangle(
    SCR_W - NAV_BTN_W / 2 + 6, cy,
    SCR_W - NAV_BTN_W / 2 - 6, cy - 7,
    SCR_W - NAV_BTN_W / 2 - 6, cy + 7,
    rCol);

  char buf[24];
  snprintf(buf, sizeof(buf), "%d/%d  %s",
           currentPage + 1, NUM_PAGES, pageNames[currentPage]);
  M5.Lcd.setTextColor(NAV_BTN_ACT, TFT_BLACK);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.drawString(buf, NAV_TITLE_X + NAV_TITLE_W / 2, cy);
  M5.Lcd.endWrite();
}

bool handleNavTap(int tx, int ty) {
  if (ty < NAV_Y) return false;
  // Circular wrap — page 0 (full-screen, no nav) is skipped
  if (tx < NAV_BTN_W) {
    currentPage = (currentPage <= 1) ? NUM_PAGES - 1 : currentPage - 1;
    drawCurrentPage(); return true;
  }
  if (tx >= SCR_W - NAV_BTN_W) {
    currentPage = (currentPage >= NUM_PAGES - 1) ? 1 : currentPage + 1;
    drawCurrentPage(); return true;
  }
  return true;
}

// ═══════════════════════════════════════════════════════════════════════════
//  PAGE 0  ─  Light selector  (2 × 2, full screen — no nav bar)
//    bw = 154 px,  bh = 114 px
// ═══════════════════════════════════════════════════════════════════════════

const char* b4Label[4] = { "A", "B", "C", "D" };
uint16_t    b4Color[4] = { TFT_RED, TFT_GREEN, TFT_BLUE, TFT_YELLOW };
bool        b4State[4] = {};

Rect b4Rect(int i) {
  int bw = (SCR_W - GAP * 3) / 2;
  int bh = (SCR_H - GAP * 3) / 2;
  return { GAP + (i % 2) * (bw + GAP), GAP + (i / 2) * (bh + GAP), bw, bh };
}

void draw4BtnPage() {
  M5.Lcd.startWrite();
  M5.Lcd.fillRect(0, 0, SCR_W, SCR_H, TFT_BLACK);
  for (int i = 0; i < 4; i++) {
    Rect r   = b4Rect(i);
    uint16_t bg = b4State[i] ? TFT_WHITE  : b4Color[i];
    uint16_t fg = b4State[i] ? b4Color[i] : TFT_WHITE;
    M5.Lcd.fillRect(r.x, r.y, r.w, r.h, bg);
    M5.Lcd.setTextColor(fg, bg);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextSize(4);
    M5.Lcd.drawString(b4Label[i], r.x + r.w / 2, r.y + r.h / 2);
  }
  M5.Lcd.endWrite();
}

void tap4Btn(int tx, int ty) {
  for (int i = 0; i < 4; i++) {
    if (inRect(tx, ty, b4Rect(i))) {
      currentPage = PAGE_CANVAS;
      drawCurrentPage();
      return;
    }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  PAGE 1  ─  6 Buttons  (3 × 2)
//    bw = 101 px,  bh = 96 px
// ═══════════════════════════════════════════════════════════════════════════

const char* b6Label[6] = { "1", "2", "3", "4", "5", "6" };
uint16_t    b6Color[6] = { TFT_RED, 0xFC00, TFT_GREEN, TFT_CYAN, TFT_BLUE, TFT_MAGENTA };
bool        b6State[6] = {};

Rect b6Rect(int i) {
  int bw = (SCR_W     - GAP * 4) / 3;
  int bh = (CONTENT_H - GAP * 3) / 2;
  return { GAP + (i % 3) * (bw + GAP), GAP + (i / 3) * (bh + GAP), bw, bh };
}

void draw6BtnPage() {
  M5.Lcd.startWrite();
  M5.Lcd.fillRect(0, 0, SCR_W, CONTENT_H, TFT_BLACK);
  for (int i = 0; i < 6; i++) {
    Rect r   = b6Rect(i);
    uint16_t bg = b6State[i] ? TFT_WHITE  : b6Color[i];
    uint16_t fg = b6State[i] ? b6Color[i] : TFT_WHITE;
    M5.Lcd.fillRect(r.x, r.y, r.w, r.h, bg);
    M5.Lcd.setTextColor(fg, bg);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextSize(3);
    M5.Lcd.drawString(b6Label[i], r.x + r.w / 2, r.y + r.h / 2);
  }
  M5.Lcd.endWrite();
}

void tap6Btn(int tx, int ty) {
  for (int i = 0; i < 6; i++) {
    if (inRect(tx, ty, b6Rect(i))) { b6State[i] = !b6State[i]; draw6BtnPage(); return; }
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  PAGE 2  ─  M5Canvas colour preview  (3 horizontal sliders)
//
//  Layout (204 px):
//    0  ..  55   colour swatch  (56 px)
//    60 .. 104   R bar          (45 px)
//   109 .. 153   G bar          (45 px)
//   158 .. 202   B bar          (45 px)
// ═══════════════════════════════════════════════════════════════════════════

#define CV_SWATCH_H  56
#define CV_BAR_START (CV_SWATCH_H + GAP)
#define CV_BAR_H     ((CONTENT_H - CV_BAR_START - GAP * 2) / 3)   // 45

void drawCanvasPage() {
  uint8_t  rv = (uint8_t)(vSlider[0] * 255);
  uint8_t  gv = (uint8_t)(vSlider[1] * 255);
  uint8_t  bv = (uint8_t)(vSlider[2] * 255);
  uint16_t sw = ((uint16_t)(rv >> 3) << 11) |
                ((uint16_t)(gv >> 2) << 5)  |
                 (uint16_t)(bv >> 3);

  canvas.fillSprite(0x18C3);

  canvas.fillRect(0, 0, SCR_W, CV_SWATCH_H, sw);
  uint16_t textCol = ((rv * 299u + gv * 587u + bv * 114u) / 1000u > 128)
                     ? TFT_BLACK : TFT_WHITE;
  char hex[10]; snprintf(hex, sizeof(hex), "#%02X%02X%02X", rv, gv, bv);
  canvas.setTextColor(textCol, sw);
  canvas.setTextDatum(MC_DATUM);
  canvas.setTextSize(3);
  canvas.drawString(hex, SCR_W / 2, CV_SWATCH_H / 2);

  const uint8_t  vals[3] = { rv, gv, bv };
  const uint16_t cols[3] = { TFT_RED, TFT_GREEN, TFT_BLUE };
  const char*    lbls[3] = { "R", "G", "B" };

  for (int i = 0; i < 3; i++) {
    int      barY = CV_BAR_START + i * (CV_BAR_H + GAP);
    int      barW = (int)(vSlider[i] * SCR_W);
    uint16_t col  = cols[i];
    int      cy   = barY + CV_BAR_H / 2;

    canvas.fillRect(0, barY, SCR_W, CV_BAR_H, 0x39E7);
    if (barW > 0)
      canvas.fillRect(0, barY, barW, CV_BAR_H, col);
    canvas.fillRect(constrain(barW - 2, 0, SCR_W - 4), barY, 4, CV_BAR_H, TFT_WHITE);

    canvas.setTextColor(TFT_WHITE, col);
    canvas.setTextDatum(ML_DATUM);
    canvas.setTextSize(2);
    canvas.drawString(lbls[i], 6, cy);

    char buf[8]; snprintf(buf, sizeof(buf), "%d", vals[i]);
    canvas.setTextColor(TFT_WHITE, 0x39E7);
    canvas.setTextDatum(MR_DATUM);
    canvas.drawString(buf, SCR_W - 6, cy);
  }

  canvas.pushSprite(0, 0);
}

void dragCanvasBar(int tx, int ty) {
  if (lockedSlider < 0) {
    for (int i = 0; i < 3; i++) {
      int barY = CV_BAR_START + i * (CV_BAR_H + GAP);
      if (ty >= barY && ty < barY + CV_BAR_H) { lockedSlider = i; break; }
    }
  }
  if (lockedSlider < 0) return;
  sliderActive          = true;
  vSlider[lockedSlider] = hBarVal(tx);
  drawCanvasPage();
}

// ═══════════════════════════════════════════════════════════════════════════
//  PAGE 3  ─  4 Vertical Sliders  (M5Canvas)
//
//  Layout (204 px):
//    4 tracks side by side, each 75 × 196 px with 4 px gaps
// ═══════════════════════════════════════════════════════════════════════════

#define VS4_COUNT  4
#define VS4_W      ((SCR_W - GAP * (VS4_COUNT + 1)) / VS4_COUNT)  // 75 px
#define VS4_H      (CONTENT_H - GAP * 2)                           // 196 px

const uint16_t vs4Color[VS4_COUNT] = { TFT_CYAN, TFT_MAGENTA, TFT_YELLOW, 0xFC00 };
const char*    vs4Label[VS4_COUNT] = { "1", "2", "3", "4" };

Rect vs4Track(int i) {
  return { GAP + i * (VS4_W + GAP), GAP, VS4_W, VS4_H };
}

void drawVSlider4Page() {
  canvas.fillSprite(0x18C3);

  for (int i = 0; i < VS4_COUNT; i++) {
    Rect     r   = vs4Track(i);
    uint16_t col = vs4Color[i];
    int      cx  = r.x + r.w / 2;
    int      fh  = (int)(vs4[i] * r.h);
    int      fy  = r.y + r.h - fh;

    // Track
    canvas.fillRect(r.x, r.y, r.w, r.h, 0x39E7);
    // Fill from bottom
    if (fh > 0)
      canvas.fillRect(r.x, fy, r.w, fh, col);
    // Thumb
    canvas.fillRect(r.x, constrain(fy - 2, r.y, r.y + r.h - 4), r.w, 4, TFT_WHITE);

    // Value % at top
    char buf[8]; snprintf(buf, sizeof(buf), "%d%%", (int)(vs4[i] * 100));
    canvas.setTextColor(TFT_WHITE, 0x39E7);
    canvas.setTextDatum(TC_DATUM);
    canvas.setTextSize(1);
    canvas.drawString(buf, cx, r.y + 3);

    // Label at bottom
    canvas.setTextColor(TFT_WHITE, 0x18C3);
    canvas.setTextDatum(BC_DATUM);
    canvas.setTextSize(2);
    canvas.drawString(vs4Label[i], cx, r.y + r.h - 2);
  }

  canvas.pushSprite(0, 0);
}

void dragVSlider4(int tx, int ty) {
  if (lockedSlider < 0) {
    for (int i = 0; i < VS4_COUNT; i++) {
      Rect r = vs4Track(i);
      if (tx >= r.x && tx < r.x + r.w) { lockedSlider = i; break; }
    }
  }
  if (lockedSlider < 0) return;
  Rect r = vs4Track(lockedSlider);
  sliderActive        = true;
  vs4[lockedSlider]   = vBarVal(ty, r.y, r.h);
  drawVSlider4Page();
}

// ═══════════════════════════════════════════════════════════════════════════
//  PAGE 4  ─  HSV Colour Wheel  (M5Canvas, pre-rendered)
//
//  X axis: Hue       left = 0°  →  right = 360°
//  Y axis: top half  = white → saturated colour  (val=255, sat 0→255)
//          bottom half = saturated → black        (sat=255, val 255→0)
//
//  Pre-render once into wheelBg (320 × CONTENT_H, PSRAM).
//  Per touch: blit wheelBg → canvas, draw cursor, pushSprite.
// ═══════════════════════════════════════════════════════════════════════════

void buildColorWheel() {
  for (int y = 0; y < CONTENT_H; y++) {
    float   yNorm = (float)y / (CONTENT_H - 1);
    uint8_t sat   = (yNorm < 0.5f) ? (uint8_t)(yNorm * 2.0f * 255) : 255;
    uint8_t val   = (yNorm < 0.5f) ? 255 : (uint8_t)((1.0f - yNorm) * 2.0f * 255);
    for (int x = 0; x < SCR_W; x++) {
      uint8_t hue = (uint8_t)((float)x / (SCR_W - 1) * 255);
      uint8_t r, g, b;
      HSVtoRGB(hue, sat, val, r, g, b);
      wheelBg.drawPixel(x, y, wheelBg.color565(r, g, b));
    }
  }
  wheelReady = true;
}

void drawColorWheelPage() {
  if (!wheelReady) buildColorWheel();

  // Blit pre-rendered wheel into working canvas
  wheelBg.pushSprite(&canvas, 0, 0);

  // Cursor screen position
  int cx = constrain((int)(cwX * (SCR_W - 1)),      0, SCR_W - 1);
  int cy = constrain((int)(cwY * (CONTENT_H - 1)),  0, CONTENT_H - 1);

  // Picked colour for cursor fill
  float   yn  = cwY;
  uint8_t ph  = (uint8_t)(cwX * 255.0f);
  uint8_t ps  = (yn < 0.5f) ? (uint8_t)(yn * 2.0f * 255) : 255;
  uint8_t pv  = (yn < 0.5f) ? 255 : (uint8_t)((1.0f - yn) * 2.0f * 255);
  uint8_t pr, pg, pb;
  HSVtoRGB(ph, ps, pv, pr, pg, pb);
  uint16_t pickedCol = canvas.color565(pr, pg, pb);

  // Cursor: plain filled circle in picked colour
  canvas.fillCircle(cx, cy, 9, pickedCol);

  canvas.pushSprite(0, 0);
}

void dragColorWheel(int tx, int ty) {
  // Edge snap — same BAR_SNAP as sliders so corners are reachable
  cwX = constrain((float)(tx - BAR_SNAP) / (SCR_W     - BAR_SNAP * 2), 0.0f, 1.0f);
  cwY = constrain((float)(ty - BAR_SNAP) / (CONTENT_H - BAR_SNAP * 2), 0.0f, 1.0f);
  drawColorWheelPage();
}

// ═══════════════════════════════════════════════════════════════════════════
//  PAGE DISPATCH
// ═══════════════════════════════════════════════════════════════════════════

void drawCurrentPage() {
  switch (currentPage) {
    case PAGE_4BTN:       draw4BtnPage();        break;
    case PAGE_6BTN:       draw6BtnPage();        break;
    case PAGE_CANVAS:     drawCanvasPage();      break;
    case PAGE_VSLDR4:     drawVSlider4Page();    break;
    case PAGE_COLORWHEEL: drawColorWheelPage();  break;
  }
  if (currentPage != PAGE_4BTN) drawNav();
}

// ═══════════════════════════════════════════════════════════════════════════
//  SETUP / LOOP
// ═══════════════════════════════════════════════════════════════════════════

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Lcd.setRotation(1);
  M5.Lcd.fillScreen(TFT_BLACK);
  canvas.createSprite(SCR_W, CONTENT_H);
  wheelBg.createSprite(SCR_W, CONTENT_H);
  drawCurrentPage();
}

void loop() {
  M5.update();

  auto t        = M5.Touch.getDetail();
  bool touching = t.isPressed();
  int  tx = t.x, ty = t.y;

  if (touching && !wasTouching) {
    touchStartX  = tx;
    touchStartY  = ty;
    isDragging   = false;
    sliderActive = false;
    lockedSlider = -1;
  }

  if (touching && wasTouching) {
    if (abs(tx - touchStartX) > DRAG_THRESH ||
        abs(ty - touchStartY) > DRAG_THRESH)
      isDragging = true;

    if (isDragging && ty < NAV_Y) {
      if (currentPage == PAGE_CANVAS)      dragCanvasBar(tx, ty);
      if (currentPage == PAGE_VSLDR4)      dragVSlider4(tx, ty);
      if (currentPage == PAGE_COLORWHEEL)  dragColorWheel(tx, ty);
    }
  }

  if (!touching && wasTouching && !isDragging) {
    if (ty >= NAV_Y && currentPage != PAGE_4BTN) {
      handleNavTap(tx, ty);
    } else {
      switch (currentPage) {
        case PAGE_4BTN:       tap4Btn(tx, ty);        break;
        case PAGE_6BTN:       tap6Btn(tx, ty);        break;
        case PAGE_CANVAS:     dragCanvasBar(tx, ty);  break;
        case PAGE_VSLDR4:     dragVSlider4(tx, ty);   break;
        case PAGE_COLORWHEEL: dragColorWheel(tx, ty); break;
      }
    }
  }

  wasTouching = touching;
}
