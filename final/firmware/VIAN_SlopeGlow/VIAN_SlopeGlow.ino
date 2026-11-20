// VIAN Slope Glow - main firmware
// Functions: fan control (Off / Sleep / Auto / Turbo), pop-up lift that follows fan mode,
// RGBW ambient lighting (groove, running ring, base halo, VIAN logo), outside 4.0" colour touch display,
// sensors, safety interlocks, 3-stage filter life (DebrisGuard, LifeHEPA, OdourCore), Wi-Fi app API.
// Status: compiles for ESP32-S3; first hardware bring-up must confirm each block.
#include <Adafruit_NeoPixel.h>
#include <TMCStepper.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include "config.h"
#include "sensors.h"
#include "display.h"

// ---------------- state ----------------
enum Mode : uint8_t { M_OFF, M_SLEEP, M_AUTO, M_TURBO };
enum Fx : uint8_t { FX_STEADY, FX_RUN, FX_BREATHE };
struct State {
  Mode mode = M_AUTO; Fx fx = FX_RUN; uint32_t color = 0xFFB36B; bool aqColor = false; bool lightOn = true;
  uint8_t bright = 60;                 // 0..100 user brightness
  float pm25 = 0, pm10 = 0, t = 0, rh = 0; int32_t voc = 100, nox = 1; float lux = 100, fanW = 0; uint16_t rpm = 0;
  int aqi = 0; uint8_t fanPct = 0; float liftMm = 0, liftTarget = 0;
  bool tilted = false, doorOpen = false, filterMissing = false, liftFault = false, night = false;
  // filters
  float hepaG = 0, hepaHours = 0, basePowerW = 0; uint8_t hepaLife = 100;
  float odourHours = 0, odourDose = 0; uint8_t odourLife = 100;
  uint32_t debrisWashedDay = 0; uint8_t debrisDays = 0;
  uint32_t modeChangedAt = 0, lastTouch = 0;
} st;

Preferences prefs;
VianUI ui;
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
uint32_t dayCounter = 0;             // run-days since first start (stored), drives the DebrisGuard reminder

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
uint32_t currentColor() { return st.aqColor ? aqiRgb(st.aqi) : st.color; }
void fillAll(Adafruit_NeoPixel &s, uint32_t c) { for (uint16_t i = 0; i < s.numPixels(); i++) s.setPixelColor(i, c); }
void lightingFrame(uint32_t now) {
  bool off = !st.lightOn || st.mode == M_OFF;
  float user = st.bright / 100.0f * (st.night ? 0.08f : 1.0f);
  uint32_t col = currentColor();
  float br = (st.fx == FX_BREATHE) ? 0.35f + 0.65f * (0.5f + 0.5f * sinf(now / 1000.0f * 1.6f)) : 1.0f;
  uint32_t base = off ? 0 : scaleColor(col, user * br);
  fillAll(ledGroove, base); fillAll(ledBase, base);
  bool ringOn = !off && st.liftMm > 3;   // running ring only when the cap is open
  for (int i = 0; i < N_RING; i++) {
    float b = 1;
    if (st.fx == FX_RUN) { float ph = fmodf((float)i / N_RING - now / 1000.0f * 0.22f + 10, 1.0f);
      float q = min(min(ph, 1 - ph), fabsf(ph - 0.5f)); b = 0.4f + 0.6f * powf(max(0.0f, 1 - q * 9), 2); }
    ledRing.setPixelColor(i, ringOn ? scaleColor(col, user * br * b) : 0);
  }
  // VIAN logo (front only) follows the strip colour; soft white is shown warm gold on the white body
  uint32_t fc = (col == 0xFFF0DA) ? 0xE3A55A : col;
  uint32_t logo = scaleColor(fc, st.night ? 0.05f : (st.mode == M_OFF ? 0.15f : max(0.3f, user)));
  for (int i = 0; i < N_FRONT; i++) ledFront.setPixelColor(i, st.lightOn ? logo : 0);
  ledGroove.show(); ledRing.show(); ledBase.show(); ledFront.show();
}

// ---------------- display ----------------
const char *modeName(Mode m) { return m == M_OFF ? "off" : m == M_SLEEP ? "sleep" : m == M_AUTO ? "auto" : "turbo"; }
const char *modeTitle(Mode m) { return m == M_OFF ? "OFF" : m == M_SLEEP ? "SLEEP" : m == M_AUTO ? "AUTO" : "TURBO"; }
void displayFrame(uint32_t now) {
  bool asleep = (st.night || st.mode == M_OFF) && now - st.lastTouch > 15000;
  ui.brightness(asleep ? 0 : (uint8_t)constrain(40 + st.lux * 0.6f, 40.0f, 255.0f));
  if (asleep) return;
  UiData d{st.aqi, st.pm25, st.pm10, st.rh, st.t, st.voc, st.fanPct, modeTitle(st.mode), WiFi.status() == WL_CONNECTED,
           currentColor(), st.hepaLife, st.odourLife, st.debrisDays,
           st.tilted || st.doorOpen || st.filterMissing,
           st.tilted ? "Unit tilted - fan off" : st.doorOpen ? "Front panel open" : "Fit the filter", (uint8_t)st.fx, st.lightOn};
  ui.draw(d);
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

// ---------------- filter reset ----------------
void resetFilters() {
  st.hepaG = 0; st.hepaHours = 0; st.basePowerW = 0; st.odourHours = 0; st.odourDose = 0; st.debrisWashedDay = dayCounter;
  prefs.putFloat("hepaG", 0); prefs.putFloat("hepaH", 0); prefs.putFloat("pw0", 0); prefs.putFloat("odH", 0); prefs.putFloat("odD", 0);
  prefs.putUInt("dgDay", dayCounter); beep(300);
}

// ---------------- on-screen touch ----------------
void touch(uint32_t now) {
  static bool was = false; static uint32_t downAt = 0; static int btn = -1; static TouchPt first;
  TouchPt t = (digitalRead(PIN_TP_INT) == LOW || was) ? touchRead() : TouchPt{false, 0, 0};
  if (t.down && !was) { downAt = now; first = t; btn = hitButton(t.x, t.y); st.lastTouch = now; }
  if (!t.down && was) {
    uint32_t held = now - downAt;
    if (btn == 0) setMode(st.mode == M_OFF ? M_AUTO : M_OFF);
    else if (btn == 1) setMode(st.mode == M_SLEEP ? M_AUTO : st.mode == M_AUTO ? M_TURBO : M_SLEEP);
    else if (btn == 2) { if (held > 1000) st.lightOn = !st.lightOn; else ui.page = ui.page == PG_LIGHT ? PG_HOME : PG_LIGHT; }
    else if (btn == 3) { if (held > 5000) resetFilters(); else ui.page = ui.page == PG_FILTER ? PG_HOME : PG_FILTER; }
    else if (ui.page == PG_LIGHT) {   // colour dots and effect buttons on the light page
      static const uint32_t sw[6] = {0xFFB36B, 0xFF7A45, 0xFFF0DA, 0x4FE3D0, 0xFF7FB0, 0x43D18F};
      if (first.y > 90 && first.y < 150) { int k = (first.x - 15) / 75; if (k >= 0 && k < 6) { st.color = sw[k]; st.aqColor = (k == 5); prefs.putUInt("color", st.color); } }
      if (first.y > 180 && first.y < 226) { int k = (first.x - 16) / 152; if (k >= 0 && k < 3) st.fx = (Fx)k; }
    }
    btn = -1;
  }
  was = t.down;
}

// ---------------- filter life (3 stages) ----------------
void filterLife(float dtH) {
  if (st.fanPct == 0) return;
  float airflow = st.fanPct * 7.0f;                                 // m3/h at fan % (CADR ~700 at 100 %), replace with lab curve
  st.hepaG += max(st.pm10, st.pm25) * 1e-6f * airflow * dtH;        // ug/m3 * m3 = ug -> g reaching the HEPA
  st.hepaHours += dtH;
  float lifeDust = 100 - 100 * st.hepaG / HEPA_CAPACITY_G, lifeTime = 100 - 100 * st.hepaHours / HEPA_MAX_HOURS, lifePower = 100;
  if (st.fanPct >= 95 && st.fanW > 0) {
    if (st.basePowerW <= 0) { st.basePowerW = st.fanW; prefs.putFloat("pw0", st.basePowerW); }
    float rise = st.fanW / st.basePowerW - 1; lifePower = 100 - 100 * constrain(rise / FILTER_POWER_RISE, 0.0f, 1.0f);
  }
  st.hepaLife = (uint8_t)constrain(min(lifeDust, min(lifeTime, lifePower)), 0.0f, 100.0f);
  st.odourHours += dtH; st.odourDose += max(0L, (long)st.voc - 100) * dtH * 3600.0f;
  st.odourLife = (uint8_t)constrain(100 - 100 * max(st.odourHours / ODOUR_MAX_HOURS, st.odourDose / ODOUR_VOC_DOSE), 0.0f, 100.0f);
  st.debrisDays = (uint8_t)min<uint32_t>(255, dayCounter - st.debrisWashedDay);
}

// ---------------- Wi-Fi app API ----------------
void apiState() {
  char j[760];
  snprintf(j, sizeof j,
    "{\"fw\":\"%s\",\"mode\":\"%s\",\"fan\":%u,\"rpm\":%u,\"lift_mm\":%.1f,\"pm25\":%.0f,\"pm10\":%.0f,\"aqi\":%d,\"t\":%.1f,\"rh\":%.0f,"
    "\"voc\":%ld,\"nox\":%ld,\"lux\":%.0f,\"fan_w\":%.1f,\"filters\":{\"debris_days\":%u,\"hepa\":%u,\"hepa_g\":%.0f,\"odour\":%u},"
    "\"light\":{\"on\":%s,\"color\":\"%06lX\",\"aq\":%s,\"fx\":%u,\"bright\":%u},"
    "\"alarms\":{\"tilt\":%s,\"door\":%s,\"no_filter\":%s,\"lift\":%s}}",
    FW_VERSION, modeName(st.mode), st.fanPct, st.rpm, st.liftMm, st.pm25, st.pm10, st.aqi, st.t, st.rh, (long)st.voc, (long)st.nox, st.lux, st.fanW,
    st.debrisDays, st.hepaLife, st.hepaG, st.odourLife,
    st.lightOn ? "true" : "false", (unsigned long)st.color, st.aqColor ? "true" : "false", st.fx, st.bright,
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
  if (http.hasArg("reset_filters")) resetFilters();
  if (http.hasArg("debris_washed")) { st.debrisWashedDay = dayCounter; prefs.putUInt("dgDay", dayCounter); }
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
  pinMode(PIN_LIFT_DIAG, INPUT); pinMode(PIN_TP_INT, INPUT_PULLUP); pinMode(PIN_TILT_INT, INPUT);
  pinMode(PIN_LIFT_STEP, OUTPUT); pinMode(PIN_LIFT_DIR, OUTPUT); pinMode(PIN_LIFT_EN, OUTPUT); digitalWrite(PIN_LIFT_EN, HIGH);
  pinMode(PIN_UV_RELAY, OUTPUT); digitalWrite(PIN_UV_RELAY, LOW); pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_FAN_TACH, INPUT); attachInterrupt(PIN_FAN_TACH, onTach, FALLING);
  busMain.begin(PIN_I2C0_SDA, PIN_I2C0_SCL, 400000); busSens.begin(PIN_I2C1_SDA, PIN_I2C1_SCL, 100000);
  lisBegin(); inaBegin(); vemlBegin(); pmsBegin(); sgpStart = millis();
  for (auto *s : {&ledGroove, &ledRing, &ledBase, &ledFront}) { s->begin(); s->setBrightness(LED_MAX_BRIGHT); s->clear(); s->show(); }
  ui.begin();
  TMCSerial.begin(115200, SERIAL_8N1, PIN_TMC_RX, PIN_TMC_TX);
  tmc.begin(); tmc.toff(4); tmc.rms_current(LIFT_RUN_CURRENT); tmc.microsteps(16); tmc.en_spreadCycle(false);   // StealthChop = silent
  tmc.pwm_autoscale(true); tmc.TCOOLTHRS(0xFFFFF); tmc.SGTHRS(LIFT_STALL_THRS);
  xTaskCreatePinnedToCore(liftTask, "lift", 4096, nullptr, 2, nullptr, 0);
  prefs.begin("vian", false);
  st.mode = (Mode)prefs.getUChar("mode", M_AUTO); st.color = prefs.getUInt("color", 0xFFB36B);
  st.hepaG = prefs.getFloat("hepaG", 0); st.hepaHours = prefs.getFloat("hepaH", 0); st.basePowerW = prefs.getFloat("pw0", 0);
  st.odourHours = prefs.getFloat("odH", 0); st.odourDose = prefs.getFloat("odD", 0);
  dayCounter = prefs.getUInt("days", 0); st.debrisWashedDay = prefs.getUInt("dgDay", 0);
  st.lastTouch = millis();
  liftHome(); wifiBegin(); beep(80);
  Serial.println(F("VIAN Slope Glow " FW_VERSION " ready"));
}

void loop() {
  uint32_t now = millis();
  static uint32_t tFast = 0, tSlow = 0, tLed = 0, tSave = 0, runMs = 0;
  pmsPoll(); http.handleClient(); touch(now);
  if (now - tFast >= 100) {                 // 10 Hz: safety + control
    tFast = now;
    st.tilted = lisTiltDeg() > TILT_LIMIT_DEG;
    st.doorOpen = digitalRead(PIN_DOOR) == HIGH;
    st.filterMissing = digitalRead(PIN_FILTER) == HIGH;
    control(now);
  }
  if (now - tLed >= 33) { tLed = now; lightingFrame(now); }   // 30 fps lights
  if (now - tSlow >= 1000) {                // 1 Hz: sensors, display, filters
    float dt = (now - tSlow) / 3600000.0f; tSlow = now;
    noInterrupts(); uint32_t p = tachPulses; tachPulses = 0; interrupts(); st.rpm = p * 30;  // 2 pulses/rev
    float t, rh; if (sht40Read(t, rh)) { st.t = t; st.rh = rh; }
    int32_t v = st.voc, n = st.nox; if (sgp41Read(st.t, st.rh, v, n)) { st.voc = v; st.nox = n; }
    if (now - pms.lastOk < 5000) { st.pm25 = pms.pm25; st.pm10 = pms.pm10; }
    st.aqi = aqiFromPm25(st.pm25);
    st.lux = vemlLux(); st.night = st.lux >= 0 && st.lux < 3 && st.mode != M_TURBO;
    st.fanW = inaWatts(); filterLife(dt);
    if (st.mode != M_OFF) { runMs += 1000; if (runMs >= 86400000UL / 2) { runMs = 0; dayCounter++; prefs.putUInt("days", dayCounter); } }  // 12 run-hours = 1 day of use
    if (st.tilted && st.mode != M_OFF) { setMode(M_OFF); beep(500); }
    displayFrame(now);
  }
  if (now - tSave >= 600000) {               // every 10 min
    tSave = now; prefs.putFloat("hepaG", st.hepaG); prefs.putFloat("hepaH", st.hepaHours);
    prefs.putFloat("odH", st.odourHours); prefs.putFloat("odD", st.odourDose);
  }
}
