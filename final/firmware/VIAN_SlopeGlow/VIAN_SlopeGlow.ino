// VIAN Slope Glow - main firmware
// Functions: fan control (Off / Sleep / Auto / Turbo), pop-up lift that follows fan mode,
// RGBW ambient lighting (groove, running ring, base halo, display backlight, VIAN logo),
// hidden segment display, touch keys, sensors, safety interlocks, filter life, Wi-Fi app API.
// Status: written against the VSG-MB rev A netlist; first hardware bring-up must confirm each block.
#include <Adafruit_NeoPixel.h>
#include <TMCStepper.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "config.h"
#include "ht1622.h"
#include "sensors.h"

// ---------------- state ----------------
enum Mode : uint8_t { M_OFF, M_SLEEP, M_AUTO, M_TURBO };
enum Fx : uint8_t { FX_STEADY, FX_RUN, FX_BREATHE };
struct State {
  Mode mode = M_AUTO; Fx fx = FX_RUN; uint32_t color = 0xFFB36B; bool aqColor = false; bool lightOn = true;
  uint8_t bright = 60;                 // 0..100 user brightness
  float pm25 = 0, t = 0, rh = 0; int32_t voc = 100, nox = 1; float lux = 100, fanW = 0; uint16_t rpm = 0;
  int aqi = 0; uint8_t fanPct = 0; float liftMm = 0, liftTarget = 0;
  bool tilted = false, doorOpen = false, filterMissing = false, liftFault = false, night = false;
  float dustG = 0, basePowerW = 0; uint8_t filterLife = 100; uint32_t modeChangedAt = 0;
} st;

Preferences prefs;
HT1622 lcd;
Adafruit_NeoPixel ledGroove(N_GROOVE, PIN_LED_GROOVE, NEO_GRBW + NEO_KHZ800);
Adafruit_NeoPixel ledRing(N_RING, PIN_LED_RING, NEO_GRBW + NEO_KHZ800);
Adafruit_NeoPixel ledBase(N_BASE, PIN_LED_BASE, NEO_GRBW + NEO_KHZ800);
Adafruit_NeoPixel ledFront(N_FRONT, PIN_LED_FRONT, NEO_GRBW + NEO_KHZ800);
HardwareSerial TMCSerial(2);
TMC2209Stepper tmc(&TMCSerial, 0.11f, 0b00);   // Rsense 110 mOhm, address 0
WebServer http(80);
volatile uint32_t tachPulses = 0;
volatile long liftPos = 0;           // steps, 0 = closed
volatile long liftGoal = 0;
volatile bool liftStall = false;

// ---------------- AQI (India CPCB PM2.5 breakpoints) ----------------
int aqiFromPm25(float c) {
  const float bp[][4] = {{0, 30, 0, 50}, {31, 60, 51, 100}, {61, 90, 101, 200}, {91, 120, 201, 300}, {121, 250, 301, 400}, {251, 500, 401, 500}};
  for (auto &b : bp) if (c <= b[1]) return (int)(b[2] + (c - b[0]) * (b[3] - b[2]) / (b[1] - b[0]));
  return 500;
}

// ---------------- fan ----------------
void IRAM_ATTR onTach() { tachPulses++; }
void fanSet(uint8_t pct) {
  const uint32_t full = (1 << FAN_PWM_BITS) - 1;
  st.fanPct = pct; uint32_t duty = (pct == 0) ? 0 : map(max<uint8_t>(pct, FAN_MIN_PCT), 0, 100, 0, full);
  ledcWrite(PIN_FAN_PWM, full - duty);   // 2N7002 open drain inverts: GPIO high pulls the fan PWM line low
}
uint8_t autoFanPct(float pm) {
  if (pm < 12) return 25; if (pm < 35) return 25 + (pm - 12) * 1.5f; if (pm < 60) return 60 + (pm - 35) * 0.8f; return 100;
}

// ---------------- lift (runs on core 0) ----------------
void liftTask(void *) {
  const uint32_t halfPeriodUs = 1000000UL / (uint32_t)(LIFT_SPEED_MM_S * LIFT_STEPS_PER_MM) / 2;
  for (;;) {
    long goal = liftGoal;
    if (goal == liftPos) { digitalWrite(PIN_LIFT_EN, HIGH); vTaskDelay(20); continue; }
    digitalWrite(PIN_LIFT_EN, LOW); bool up = goal > liftPos; digitalWrite(PIN_LIFT_DIR, up);
    for (int i = 0; i < 200 && liftPos != goal; i++) {
      if (!up && digitalRead(PIN_LIFT_HOME) == LOW) { liftPos = 0; liftGoal = 0; break; }
      if (digitalRead(PIN_LIFT_DIAG) == HIGH) {   // StallGuard: something is blocking the cap
        liftStall = true;                           // latched until the next mode change
        liftGoal = up ? liftPos : min<long>(liftPos + 5 * LIFT_STEPS_PER_MM, LIFT_MAX_MM * LIFT_STEPS_PER_MM);  // closing: back off 5 mm (anti-pinch)
        break;
      }
      digitalWrite(PIN_LIFT_STEP, HIGH); delayMicroseconds(halfPeriodUs);
      digitalWrite(PIN_LIFT_STEP, LOW); delayMicroseconds(halfPeriodUs);
      liftPos += up ? 1 : -1;
    }
    taskYIELD();
  }
}
void liftHome() {  // close until home switch, at power-up
  liftPos = LIFT_MAX_MM * LIFT_STEPS_PER_MM; liftGoal = -LIFT_STEPS_PER_MM * 5;
  uint32_t t0 = millis(); while (digitalRead(PIN_LIFT_HOME) != LOW && millis() - t0 < 15000) delay(10);
  liftPos = 0; liftGoal = 0; if (millis() - t0 >= 15000) st.liftFault = true;
}
void liftTo(float mm) {
  mm = constrain(mm, 0, LIFT_MAX_MM); st.liftTarget = mm;
  if (!st.liftFault && !liftStall) liftGoal = (long)(mm * LIFT_STEPS_PER_MM);
}

// ---------------- lighting ----------------
uint32_t scaleColor(uint32_t rgb, float k) {
  uint8_t r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
  uint8_t w = 0;
  if (r > 200 && g > 150 && b > 80) { w = min(r, min(g, b)); r -= w; g -= w; b -= w; }  // warm/soft white -> W channel
  return Adafruit_NeoPixel::Color(r * k, g * k, b * k, w * k);
}
uint32_t aqColor(int aqi) { return aqi <= 50 ? 0x43D18F : aqi <= 100 ? 0xA6D84B : aqi <= 200 ? 0xF2B33D : 0xFF5A4F; }
void fillAll(Adafruit_NeoPixel &s, uint32_t c) { for (uint16_t i = 0; i < s.numPixels(); i++) s.setPixelColor(i, c); }
void lightingFrame(uint32_t now) {
  bool off = !st.lightOn || st.mode == M_OFF;
  float user = st.bright / 100.0f * (st.night ? 0.08f : 1.0f);
  uint32_t col = st.aqColor ? aqColor(st.aqi) : st.color;
  float br = (st.fx == FX_BREATHE) ? 0.35f + 0.65f * (0.5f + 0.5f * sinf(now / 1000.0f * 1.6f)) : 1.0f;
  uint32_t base = off ? 0 : scaleColor(col, user * br);
  fillAll(ledGroove, base); fillAll(ledBase, base);
  // running ring, only lit when the cap is open more than 3 mm
  bool ringOn = !off && st.liftMm > 3;
  for (int i = 0; i < N_RING; i++) {
    float b = 1;
    if (st.fx == FX_RUN) { float ph = fmodf((float)i / N_RING - now / 1000.0f * 0.22f + 10, 1.0f);
      float q = min(min(ph, 1 - ph), fabsf(ph - 0.5f)); b = 0.4f + 0.6f * powf(max(0.0f, 1 - q * 9), 2); }
    ledRing.setPixelColor(i, ringOn ? scaleColor(col, user * br * b) : 0);
  }
  // display backlight + logo follow the strip colour; soft white is shown warm gold on the white body
  uint32_t fc = (col == 0xFFF0DA) ? 0xE3A55A : col;
  uint32_t disp = (st.mode == M_OFF) ? 0 : scaleColor(fc, st.night ? 0.05f : max(0.25f, user));
  uint32_t logo = scaleColor(fc, st.night ? 0.05f : (st.mode == M_OFF ? 0.15f : max(0.3f, user)));
  for (int i = 0; i < N_DISP_BL; i++) ledFront.setPixelColor(i, disp);
  for (int i = N_DISP_BL; i < N_FRONT; i++) ledFront.setPixelColor(i, st.lightOn ? logo : 0);
  ledGroove.show(); ledRing.show(); ledBase.show(); ledFront.show();
}

// ---------------- display ----------------
void displayFrame() {
  lcd.clear();
  if (st.mode != M_OFF && !st.night) {
    lcdNumber(lcd, D_AQI0, 3, st.aqi); lcdDial(lcd, (uint8_t)min(20, st.aqi * 20 / 300));
    lcdNumber(lcd, D_PM0, 3, (int)st.pm25); lcdNumber(lcd, D_RH0, 2, (int)st.rh);
    lcdNumber(lcd, D_DB0, 2, st.fanPct < 30 ? 28 : st.fanPct < 70 ? 41 : 52);   // dB from lab table, replace after test
    lcdIcon(lcd, I_LBL_AQI, 1); lcdIcon(lcd, I_LBL_PM, 1); lcdIcon(lcd, I_LBL_UGM3, 1); lcdIcon(lcd, I_LBL_HUM, 1);
    lcdIcon(lcd, I_LBL_PCT, 1); lcdIcon(lcd, I_LBL_DB, 1);
    lcdIcon(lcd, I_GOOD, st.aqi <= 100); lcdIcon(lcd, I_MODERATE, st.aqi > 100 && st.aqi <= 200); lcdIcon(lcd, I_POOR, st.aqi > 200);
    lcdIcon(lcd, I_AUTO, st.mode == M_AUTO); lcdIcon(lcd, I_SLEEP, st.mode == M_SLEEP); lcdIcon(lcd, I_TURBO, st.mode == M_TURBO);
    lcdIcon(lcd, I_WIFI, WiFi.status() == WL_CONNECTED);
  }
  lcdIcon(lcd, I_FILTER_WARN, st.filterMissing || st.filterLife < 10 || st.doorOpen);
  lcdIcon(lcd, I_FILTER, st.filterLife < 10);
  lcd.flush();
}

// ---------------- control ----------------
void beep(int ms) { tone(PIN_BUZZER, 2700, ms); }
void setMode(Mode m) { if (m != st.mode) { st.mode = m; st.modeChangedAt = millis(); liftStall = false; prefs.putUChar("mode", m); beep(40); } }
void control(uint32_t now) {
  bool blocked = st.tilted || st.doorOpen || st.filterMissing;
  uint8_t pct = 0;
  if (!blocked) switch (st.mode) {
    case M_OFF: pct = 0; break; case M_SLEEP: pct = FAN_SLEEP_PCT; break;
    case M_AUTO: pct = autoFanPct(st.pm25); break; case M_TURBO: pct = FAN_TURBO_PCT; break;
  }
  fanSet(pct);
  // lift height follows fan mode; Auto waits until the mode is stable so the cap does not hunt
  float h = 0;
  if (!blocked) h = st.mode == M_SLEEP ? LIFT_H_SLEEP : st.mode == M_AUTO ? LIFT_H_AUTO : st.mode == M_TURBO ? LIFT_H_TURBO : 0;
  bool stable = (now - st.modeChangedAt) > (st.mode == M_AUTO ? LIFT_SETTLE_MS : 1500);
  if (h == 0 || stable) if (fabsf(h - st.liftTarget) > 0.5f) liftTo(h);
  st.liftMm = liftPos / (float)LIFT_STEPS_PER_MM;
  digitalWrite(PIN_UV_RELAY, (!blocked && st.mode != M_OFF && pct > 0) ? HIGH : LOW);   // Hospital / Pro build only
}

// ---------------- touch keys ----------------
void touch(uint32_t now) {
  static uint8_t last = 0; static uint32_t downAt = 0;
  if (digitalRead(PIN_QT_CHANGE) == HIGH && last == 0) return;   // CHANGE goes low on any key event
  uint8_t k = qtKeys();
  if (k && !last) downAt = now;
  if (!k && last) {
    uint32_t held = now - downAt;
    if (last & 1) setMode(st.mode == M_OFF ? M_AUTO : M_OFF);
    if ((last & 2) && held >= 5000) { st.dustG = 0; st.basePowerW = 0; prefs.putFloat("dust", 0); prefs.putFloat("pw0", 0); beep(300); }  // filter reset
    else if (last & 2) setMode(st.mode == M_SLEEP ? M_AUTO : st.mode == M_AUTO ? M_TURBO : M_SLEEP);
    if (last & 4) { if (held > 800) st.lightOn = !st.lightOn; else st.fx = (Fx)((st.fx + 1) % 3); }
  }
  last = k;
}

// ---------------- filter life ----------------
void filterLife(float dtH) {
  float airflow = st.fanPct * 7.0f;                       // m3/h at fan % (CADR ~700 at 100 %), replace with lab curve
  st.dustG += st.pm25 * 1e-6f * airflow * dtH;            // ug/m3 * m3 = ug -> g
  float lifeDust = 100 - 100 * st.dustG / FILTER_CAPACITY_G;
  float lifePower = 100;
  if (st.fanPct >= 60 && st.fanW > 0) {
    if (st.basePowerW <= 0) { st.basePowerW = st.fanW; prefs.putFloat("pw0", st.basePowerW); }
    float rise = st.fanW / st.basePowerW - 1; lifePower = 100 - 100 * constrain(rise / FILTER_POWER_RISE, 0.0f, 1.0f);
  }
  st.filterLife = (uint8_t)constrain(min(lifeDust, lifePower), 0.0f, 100.0f);
}

// ---------------- Wi-Fi app API ----------------
const char *modeName(Mode m) { return m == M_OFF ? "off" : m == M_SLEEP ? "sleep" : m == M_AUTO ? "auto" : "turbo"; }
void apiState() {
  char j[640];
  snprintf(j, sizeof j,
    "{\"fw\":\"%s\",\"mode\":\"%s\",\"fan\":%u,\"rpm\":%u,\"lift_mm\":%.1f,\"pm25\":%.0f,\"aqi\":%d,\"t\":%.1f,\"rh\":%.0f,"
    "\"voc\":%ld,\"nox\":%ld,\"lux\":%.0f,\"fan_w\":%.1f,\"filter_life\":%u,\"light\":{\"on\":%s,\"color\":\"%06lX\",\"aq\":%s,\"fx\":%u,\"bright\":%u},"
    "\"alarms\":{\"tilt\":%s,\"door\":%s,\"no_filter\":%s,\"lift\":%s}}",
    FW_VERSION, modeName(st.mode), st.fanPct, st.rpm, st.liftMm, st.pm25, st.aqi, st.t, st.rh, (long)st.voc, (long)st.nox, st.lux, st.fanW,
    st.filterLife, st.lightOn ? "true" : "false", (unsigned long)st.color, st.aqColor ? "true" : "false", st.fx, st.bright,
    st.tilted ? "true" : "false", st.doorOpen ? "true" : "false", st.filterMissing ? "true" : "false", (st.liftFault || liftStall) ? "true" : "false");
  http.send(200, "application/json", j);
}
void apiSet() {
  if (http.hasArg("mode")) { String m = http.arg("mode"); setMode(m == "off" ? M_OFF : m == "sleep" ? M_SLEEP : m == "turbo" ? M_TURBO : M_AUTO); }
  if (http.hasArg("color")) { st.color = strtoul(http.arg("color").c_str(), nullptr, 16) & 0xFFFFFF; st.aqColor = false; prefs.putUInt("color", st.color); }
  if (http.hasArg("aq")) st.aqColor = http.arg("aq") == "1";
  if (http.hasArg("fx")) st.fx = (Fx)constrain(http.arg("fx").toInt(), 0, 2);
  if (http.hasArg("bright")) st.bright = constrain(http.arg("bright").toInt(), 0, 100);
  if (http.hasArg("light")) st.lightOn = http.arg("light") == "1";
  if (http.hasArg("ssid")) { prefs.putString("ssid", http.arg("ssid")); prefs.putString("pass", http.arg("pass")); http.send(200, "text/plain", "saved, rebooting"); delay(300); ESP.restart(); }
  apiState();
}
void wifiBegin() {
  String ssid = prefs.getString("ssid", ""), pass = prefs.getString("pass", "");
  if (ssid.length()) { WiFi.mode(WIFI_STA); WiFi.begin(ssid.c_str(), pass.c_str()); }
  else { WiFi.mode(WIFI_AP); String ap = AP_PREFIX + String((uint32_t)ESP.getEfuseMac(), HEX); WiFi.softAP(ap.c_str(), AP_PASS); }
  http.on("/api/state", HTTP_GET, apiState); http.on("/api/set", HTTP_ANY, apiSet); http.begin();
}

// ---------------- setup / loop ----------------
void setup() {
  ledcAttach(PIN_FAN_PWM, FAN_PWM_HZ, FAN_PWM_BITS); fanSet(0);   // first: hold the fan stopped
  Serial.begin(115200);
  pinMode(PIN_DOOR, INPUT_PULLUP); pinMode(PIN_FILTER, INPUT_PULLUP); pinMode(PIN_LIFT_HOME, INPUT_PULLUP);
  pinMode(PIN_LIFT_DIAG, INPUT); pinMode(PIN_QT_CHANGE, INPUT_PULLUP); pinMode(PIN_TILT_INT, INPUT);
  pinMode(PIN_LIFT_STEP, OUTPUT); pinMode(PIN_LIFT_DIR, OUTPUT); pinMode(PIN_LIFT_EN, OUTPUT); digitalWrite(PIN_LIFT_EN, HIGH);
  pinMode(PIN_UV_RELAY, OUTPUT); digitalWrite(PIN_UV_RELAY, LOW); pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_FAN_TACH, INPUT); attachInterrupt(PIN_FAN_TACH, onTach, FALLING);
  busMain.begin(PIN_I2C0_SDA, PIN_I2C0_SCL, 400000); busSens.begin(PIN_I2C1_SDA, PIN_I2C1_SCL, 100000);
  lisBegin(); inaBegin(); vemlBegin(); pmsBegin(); sgpStart = millis();
  for (auto *s : {&ledGroove, &ledRing, &ledBase, &ledFront}) { s->begin(); s->setBrightness(LED_MAX_BRIGHT); s->clear(); s->show(); }
  lcd.begin();
  TMCSerial.begin(115200, SERIAL_8N1, PIN_TMC_RX, PIN_TMC_TX);
  tmc.begin(); tmc.toff(4); tmc.rms_current(LIFT_RUN_CURRENT); tmc.microsteps(16); tmc.en_spreadCycle(false);   // StealthChop = silent
  tmc.pwm_autoscale(true); tmc.TCOOLTHRS(0xFFFFF); tmc.SGTHRS(LIFT_STALL_THRS);
  xTaskCreatePinnedToCore(liftTask, "lift", 4096, nullptr, 2, nullptr, 0);
  prefs.begin("vian", false);
  st.mode = (Mode)prefs.getUChar("mode", M_AUTO); st.color = prefs.getUInt("color", 0xFFB36B);
  st.dustG = prefs.getFloat("dust", 0); st.basePowerW = prefs.getFloat("pw0", 0);
  liftHome(); wifiBegin(); beep(80);
  Serial.println(F("VIAN Slope Glow " FW_VERSION " ready"));
}

void loop() {
  uint32_t now = millis();
  static uint32_t tFast = 0, tSlow = 0, tLed = 0, tSave = 0;
  pmsPoll(); http.handleClient(); touch(now);
  if (now - tFast >= 100) {                 // 10 Hz: safety + control
    tFast = now;
    st.tilted = lisTiltDeg() > TILT_LIMIT_DEG;
    st.doorOpen = digitalRead(PIN_DOOR) == HIGH;
    st.filterMissing = digitalRead(PIN_FILTER) == HIGH;
    control(now);
  }
  if (now - tLed >= 33) { tLed = now; lightingFrame(now); }   // 30 fps lights
  if (now - tSlow >= 1000) {                // 1 Hz: sensors, display, filter
    float dt = (now - tSlow) / 3600000.0f; tSlow = now;
    noInterrupts(); uint32_t p = tachPulses; tachPulses = 0; interrupts(); st.rpm = p * 30;  // 2 pulses/rev
    float t, rh; if (sht40Read(t, rh)) { st.t = t; st.rh = rh; }
    int32_t v = st.voc, n = st.nox; if (sgp41Read(st.t, st.rh, v, n)) { st.voc = v; st.nox = n; }
    if (now - pms.lastOk < 5000) st.pm25 = pms.pm25;
    st.aqi = aqiFromPm25(st.pm25);
    st.lux = vemlLux(); st.night = st.lux >= 0 && st.lux < 3 && st.mode != M_TURBO;
    st.fanW = inaWatts(); filterLife(dt);
    if (st.tilted && st.mode != M_OFF) { setMode(M_OFF); beep(500); }
    displayFrame();
  }
  if (now - tSave >= 600000) { tSave = now; prefs.putFloat("dust", st.dustG); }   // every 10 min
}
