// VIAN Home 700 screen: 4.0-inch IPS 480 x 320 (ST7796S, SPI) mounted landscape behind black-printed glass.
// Only the centre 480 x 264 px (86 x 44 mm) shows through the glass window. Not a touch screen:
// pages are changed with the touch icons on the pop-up top (AT42QT1070, see sensors.h).
// Drawn into a PSRAM sprite and pushed in one go (no flicker). True-black background blends with the glass.
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "config.h"

class VianLCD : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796 panel; lgfx::Bus_SPI bus; lgfx::Light_PWM light;
 public:
  VianLCD() {
    { auto c = bus.config(); c.spi_host = SPI2_HOST; c.spi_mode = 0; c.freq_write = 40000000; c.freq_read = 16000000;
      c.pin_sclk = PIN_LCD_SCK; c.pin_mosi = PIN_LCD_MOSI; c.pin_miso = -1; c.pin_dc = PIN_LCD_DC; c.dma_channel = SPI_DMA_CH_AUTO; bus.config(c); panel.setBus(&bus); }
    { auto c = panel.config(); c.pin_cs = PIN_LCD_CS; c.pin_rst = -1;   // reset is tied to ESP_EN on the board
      c.panel_width = 320; c.panel_height = 480; c.readable = false; c.invert = true; c.rgb_order = false; c.bus_shared = false; panel.config(c); }
    { auto c = light.config(); c.pin_bl = PIN_LCD_BL; c.invert = false; c.freq = 22000; c.pwm_channel = 7; light.config(c); panel.setLight(&light); }
    setPanel(&panel);
  }
};

// ---------- the 9 pages ----------
enum PageId : uint8_t { P_AQI, P_PM25, P_PM10, P_HUM, P_VOC, P_FAN, P_FILTER, P_LIGHT, P_WIFI, P_COUNT };
static const char *PAGE_TITLE[P_COUNT] = {"AIR QUALITY", "PM2.5", "PM10", "HUMIDITY", "GASES (VOC)", "FAN", "FILTER", "LIGHT", "WI-FI"};
struct UiData {
  int aqi; float pm25, pm10, rh, t; int32_t voc; uint8_t fanPct; const char *mode; uint8_t dB; bool wifi; uint8_t units;
  uint32_t accent; uint8_t hepa, odour, debrisDays; bool alarm; const char *alarmText; const char *fxName; uint8_t bright; bool locked;
};
static uint16_t c565(uint32_t rgb) { return lgfx::color565((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); }
static uint32_t aqiRgb(int a) { return a <= 50 ? 0x43D18F : a <= 100 ? 0xA6D84B : a <= 200 ? 0xF2B33D : 0xFF5A4F; }
static const char *aqiWord(int a) { return a <= 50 ? "GOOD" : a <= 100 ? "SATISFACTORY" : a <= 200 ? "MODERATE" : a <= 300 ? "POOR" : "VERY POOR"; }

class VianUI {
 public:
  VianLCD lcd; LGFX_Sprite spr{&lcd}; uint8_t page = P_AQI;
  static const int Y0 = 28, H = 264;   // visible window inside the 320 px panel height
  void begin() { lcd.init(); lcd.setRotation(1); lcd.setBrightness(0); lcd.fillScreen(TFT_BLACK); spr.setPsram(true); spr.setColorDepth(16); spr.createSprite(480, 320); }
  void brightness(uint8_t b) { lcd.setBrightness(b); }
  void next(int d) { page = (page + P_COUNT + d) % P_COUNT; }
  void draw(const UiData &d) {
    const uint16_t fg = TFT_WHITE, mut = c565(0x8A929C), acc = c565(d.accent);
    spr.fillSprite(TFT_BLACK); spr.setTextDatum(textdatum_t::middle_center);
    char big[16] = "", sub[48] = ""; uint16_t subc = mut;
    switch (page) {
      case P_AQI: snprintf(big, sizeof big, "%d", d.aqi); snprintf(sub, sizeof sub, "AQI  %s", aqiWord(d.aqi)); subc = c565(aqiRgb(d.aqi)); break;
      case P_PM25: snprintf(big, sizeof big, "%.0f", d.pm25); snprintf(sub, sizeof sub, "ug/m3  fine dust"); break;
      case P_PM10: snprintf(big, sizeof big, "%.0f", d.pm10); snprintf(sub, sizeof sub, "ug/m3  coarse dust"); break;
      case P_HUM: snprintf(big, sizeof big, "%.0f%%", d.rh); snprintf(sub, sizeof sub, "%.0f C  %s", d.t, d.rh > 70 ? "humid" : d.rh < 30 ? "dry" : "comfortable"); break;
      case P_VOC: snprintf(big, sizeof big, "%s", d.voc < 150 ? "LOW" : d.voc < 250 ? "MID" : "HIGH"); snprintf(sub, sizeof sub, "smells and fumes  index %ld", (long)d.voc); break;
      case P_FAN: snprintf(big, sizeof big, "%s", d.mode); snprintf(sub, sizeof sub, "fan speed %u %%", d.fanPct); break;
      case P_FILTER: snprintf(big, sizeof big, "%u%%", d.hepa); snprintf(sub, sizeof sub, d.debrisDays >= DEBRIS_WASH_DAYS ? "rinse the mesh now" : "carbon %u %%  rinse mesh in %d days", d.odour, DEBRIS_WASH_DAYS - d.debrisDays); break;
      case P_LIGHT: snprintf(big, sizeof big, "%s", d.fxName); snprintf(sub, sizeof sub, "brightness %u %%", d.bright); subc = acc; break;
      case P_WIFI: snprintf(big, sizeof big, "%s", d.wifi ? "ON" : "OFF"); snprintf(sub, sizeof sub, d.wifi ? "app connected  %u VIAN in home" : "open the VIAN app to connect", d.units); break;
    }
    spr.setFont(&fonts::FreeSansBold9pt7b); spr.setTextColor(mut); spr.drawString(PAGE_TITLE[page], 240, Y0 + 24);
    spr.setFont(&fonts::FreeSansBold24pt7b); spr.setTextColor(fg); spr.setTextSize(3);   // about 25-28 mm tall
    if (spr.textWidth(big) > 460) spr.setTextSize(2);                                      // long words (RUNNING, BREATHE) still fit
    spr.drawString(big, 240, Y0 + 128); spr.setTextSize(1);
    spr.setFont(&fonts::FreeSansBold12pt7b); spr.setTextColor(subc); spr.drawString(sub, 240, Y0 + 214);
    for (int k = 0; k < P_COUNT; k++) spr.fillCircle(240 + (k - (P_COUNT - 1) / 2.0f) * 16, Y0 + 248, 4, k == page ? fg : c565(0x3A3F45));
    if (d.locked) { spr.setFont(&fonts::Font2); spr.setTextColor(mut); spr.drawString("LOCKED", 440, Y0 + 24); }
    if (d.alarm) { spr.fillRoundRect(60, Y0 + 92, 360, 72, 12, c565(0xFF5A4F)); spr.setTextColor(TFT_WHITE); spr.setFont(&fonts::FreeSansBold12pt7b); spr.drawString(d.alarmText, 240, Y0 + 128); }
    spr.pushSprite(0, 0);
  }
};
