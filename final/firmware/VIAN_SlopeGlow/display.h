// Outside colour touch display: 4.0-inch IPS 480 x 320, ST7796S over SPI, FT6336U capacitive touch on I2C0.
// Drawn into a PSRAM sprite and pushed in one go (no flicker).
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Wire.h>
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

// ---------- FT6336U touch (I2C0, 0x38) ----------
struct TouchPt { bool down; int16_t x, y; };
static TouchPt touchRead() {
  TouchPt t{false, 0, 0};
  Wire.beginTransmission(0x38); Wire.write(0x02); if (Wire.endTransmission(false) != 0) return t;
  if (Wire.requestFrom((uint8_t)0x38, (uint8_t)5) != 5) return t;
  uint8_t n = Wire.read() & 0x0F, xh = Wire.read(), xl = Wire.read(), yh = Wire.read(), yl = Wire.read();
  if (n == 0 || n > 2) return t;
  int16_t rx = ((xh & 0x0F) << 8) | xl, ry = ((yh & 0x0F) << 8) | yl;   // panel portrait 320 x 480
  t.down = true; t.x = ry; t.y = 319 - rx;                                // landscape rotation 1; confirm on first module
  return t;
}

// ---------- UI ----------
enum Page : uint8_t { PG_HOME, PG_LIGHT, PG_FILTER };
struct UiData {
  int aqi; float pm25, pm10, rh, t; int32_t voc; uint8_t fanPct; const char *mode; bool wifi;
  uint32_t accent; uint8_t hepa, odour, debrisDays; bool alarm; const char *alarmText; uint8_t fx; bool lightOn;
};
static uint16_t c565(uint32_t rgb) { return lgfx::color565((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); }
static uint32_t aqiRgb(int a) { return a <= 50 ? 0x43D18F : a <= 100 ? 0xA6D84B : a <= 200 ? 0xF2B33D : 0xFF5A4F; }
static const char *aqiWord(int a) { return a <= 50 ? "GOOD" : a <= 100 ? "SATISFACTORY" : a <= 200 ? "MODERATE" : a <= 300 ? "POOR" : "VERY POOR"; }

struct Button { int16_t x, y, w, h; const char *label; };
static const Button BTN[4] = {{8, 262, 112, 50, "Power"}, {124, 262, 112, 50, "Mode"}, {240, 262, 112, 50, "Light"}, {356, 262, 116, 50, "Filter"}};
static int hitButton(int16_t x, int16_t y) { for (int i = 0; i < 4; i++) if (x >= BTN[i].x && x < BTN[i].x + BTN[i].w && y >= BTN[i].y && y < BTN[i].y + BTN[i].h) return i; return -1; }

class VianUI {
 public:
  VianLCD lcd; LGFX_Sprite spr{&lcd}; Page page = PG_HOME;
  void begin() {
    lcd.init(); lcd.setRotation(1); lcd.setBrightness(0); lcd.fillScreen(TFT_BLACK);
    spr.setPsram(true); spr.setColorDepth(16); spr.createSprite(480, 320);
  }
  void brightness(uint8_t b) { lcd.setBrightness(b); }
  void draw(const UiData &d) {
    const uint16_t bg = 0x0841, fg = TFT_WHITE, mut = 0x8C71, acc = c565(d.accent);
    spr.fillSprite(bg); spr.setTextDatum(textdatum_t::top_left);
    // top bar
    spr.setFont(&fonts::Font2); spr.setTextColor(mut);
    spr.drawString(d.mode, 12, 8); spr.drawString(d.wifi ? "Wi-Fi" : "Wi-Fi off", 200, 8);
    char b[48]; snprintf(b, sizeof b, "Filter %u%%", d.hepa); spr.drawRightString(b, 468, 8);
    if (page == PG_HOME) home(d, fg, mut, acc); else if (page == PG_LIGHT) light(d, fg, mut, acc); else filter(d, fg, mut, acc);
    if (d.alarm) { spr.fillRoundRect(90, 110, 300, 60, 10, c565(0xFF5A4F)); spr.setTextColor(TFT_WHITE); spr.setFont(&fonts::FreeSansBold12pt7b);
      spr.setTextDatum(textdatum_t::middle_center); spr.drawString(d.alarmText, 240, 140); spr.setTextDatum(textdatum_t::top_left); }
    // buttons
    spr.setFont(&fonts::FreeSans9pt7b);
    for (int i = 0; i < 4; i++) { const Button &k = BTN[i]; bool on = (i == 2 && page == PG_LIGHT) || (i == 3 && page == PG_FILTER);
      spr.fillRoundRect(k.x, k.y, k.w, k.h, 12, on ? acc : 0x2124); spr.setTextColor(on ? TFT_BLACK : fg);
      spr.setTextDatum(textdatum_t::middle_center); spr.drawString(k.label, k.x + k.w / 2, k.y + k.h / 2); }
    spr.setTextDatum(textdatum_t::top_left);
    spr.pushSprite(0, 0);
  }
 private:
  void home(const UiData &d, uint16_t fg, uint16_t mut, uint16_t acc) {
    uint16_t ac = c565(aqiRgb(d.aqi)); int ang = 135 + min(d.aqi, 500) * 270 / 500;
    spr.fillArc(130, 145, 100, 86, 135, 405, 0x2124); spr.fillArc(130, 145, 100, 86, 135, ang, ac);
    spr.setTextDatum(textdatum_t::middle_center); spr.setTextColor(fg); spr.setFont(&fonts::FreeSansBold24pt7b);
    spr.drawNumber(d.aqi, 130, 135); spr.setFont(&fonts::FreeSans9pt7b); spr.setTextColor(ac); spr.drawString(aqiWord(d.aqi), 130, 178);
    spr.setTextColor(mut); spr.drawString("AQI", 130, 98); spr.setTextDatum(textdatum_t::top_left);
    row(260, 44, "PM2.5", d.pm25, "ug/m3", fg, mut); row(260, 92, "PM10", d.pm10, "ug/m3", fg, mut);
    row(260, 140, "Humidity", d.rh, "%", fg, mut); row(260, 188, "Temp", d.t, "C", fg, mut);
    spr.fillRoundRect(260, 236, 200, 4, 2, acc);
  }
  void row(int x, int y, const char *l, float v, const char *u, uint16_t fg, uint16_t mut) {
    spr.setFont(&fonts::Font2); spr.setTextColor(mut); spr.drawString(l, x, y);
    spr.setFont(&fonts::FreeSansBold18pt7b); spr.setTextColor(fg); char b[16]; snprintf(b, sizeof b, "%.0f", v); spr.drawString(b, x, y + 14);
    int w = spr.textWidth(b); spr.setFont(&fonts::Font2); spr.setTextColor(mut); spr.drawString(u, x + w + 6, y + 28);
  }
  void light(const UiData &d, uint16_t fg, uint16_t mut, uint16_t acc) {
    spr.setFont(&fonts::FreeSansBold12pt7b); spr.setTextColor(fg); spr.drawString("Ambient light", 16, 40);
    static const uint32_t sw[6] = {0xFFB36B, 0xFF7A45, 0xFFF0DA, 0x4FE3D0, 0xFF7FB0, 0x43D18F};
    for (int i = 0; i < 6; i++) { spr.fillCircle(50 + i * 75, 120, 26, c565(sw[i])); if (sw[i] == d.accent) spr.drawCircle(50 + i * 75, 120, 31, fg); }
    static const char *fx[3] = {"Steady", "Running", "Breathing"};
    spr.setFont(&fonts::FreeSans9pt7b);
    for (int i = 0; i < 3; i++) { spr.fillRoundRect(16 + i * 152, 180, 140, 46, 10, i == d.fx ? acc : 0x2124);
      spr.setTextColor(i == d.fx ? TFT_BLACK : fg); spr.setTextDatum(textdatum_t::middle_center); spr.drawString(fx[i], 86 + i * 152, 203); }
    spr.setTextDatum(textdatum_t::top_left); spr.setTextColor(mut); spr.setFont(&fonts::Font2);
    spr.drawString(d.lightOn ? "Hold Light 1 s to switch lights off" : "Lights are off - hold Light 1 s to switch on", 16, 236);
  }
  void filter(const UiData &d, uint16_t fg, uint16_t mut, uint16_t acc) {
    spr.setFont(&fonts::FreeSansBold12pt7b); spr.setTextColor(fg); spr.drawString("Filters", 16, 36);
    bar(70, "DebrisGuard (washable)", d.debrisDays >= 21 ? 0 : 100 - d.debrisDays * 100 / 21, d.debrisDays >= 21 ? "Rinse now" : "OK", fg, mut, acc);
    bar(130, "LifeHEPA 5-year H13", d.hepa, d.hepa < 10 ? "Change soon" : "OK", fg, mut, acc);
    bar(190, "OdourCore carbon", d.odour, d.odour < 10 ? "Change soon" : "OK", fg, mut, acc);
    spr.setFont(&fonts::Font2); spr.setTextColor(mut); spr.drawString("Hold Filter 5 s after changing or rinsing to reset", 16, 240);
  }
  void bar(int y, const char *l, int pct, const char *st, uint16_t fg, uint16_t mut, uint16_t acc) {
    spr.setFont(&fonts::FreeSans9pt7b); spr.setTextColor(fg); spr.drawString(l, 16, y);
    char b[24]; snprintf(b, sizeof b, "%d%%  %s", pct, st); spr.setTextColor(mut); spr.drawRightString(b, 464, y);
    spr.fillRoundRect(16, y + 26, 448, 10, 5, 0x2124); spr.fillRoundRect(16, y + 26, max(10, 448 * pct / 100), 10, 5, pct < 10 ? c565(0xFF5A4F) : acc);
  }
};
