#include "M5Unified.h"
#include "M5GFX.h"

static int32_t w;
static int32_t h;
static bool drawed = false;

void setup()
{
    auto cfg = M5.config();
    M5.begin(cfg);
    w = M5.Lcd.width();
    h = M5.Lcd.height();
    
    M5.Lcd.fillScreen(WHITE);
    M5.Display.setRotation(1);
    M5.Display.setTextColor(TFT_BLACK);
    M5.Display.setTextDatum(top_center);
    M5.Display.drawString("Button Released", w / 2, 0, &fonts::FreeMonoBold12pt7b);
}

void loop()
{
    M5.update();
    if(M5.BtnA.isPressed() || M5.BtnB.isPressed() || M5.BtnC.isPressed())
    {
        if (!drawed){
            M5.Lcd.fillScreen(WHITE);
        }
        M5.Display.drawString("Button  Detail:", w / 2, 0, &fonts::FreeMonoBold12pt7b);
        if (M5.BtnA.isPressed()) {
            M5.Display.drawString("ButtonA Pressed", w / 2, 30, &fonts::FreeMonoBold12pt7b);
        }
        else if (M5.BtnB.isPressed()) {
            M5.Display.drawString("ButtonB Pressed", w / 2, 60, &fonts::FreeMonoBold12pt7b);
        }
        else if (M5.BtnC.isPressed()) {
            M5.Display.drawString("ButtonC Pressed", w / 2, 90, &fonts::FreeMonoBold12pt7b);
        }
        drawed = true;
    }
    else if (drawed){
        drawed = false;
        M5.Display.clear(WHITE);
        M5.Display.drawString("Button Released", w / 2, 0, &fonts::FreeMonoBold12pt7b);
    } 
    vTaskDelay(1);
}  