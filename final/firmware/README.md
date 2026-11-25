# VIAN Slope Glow firmware (VSG-1.0.0)

Runs on the VSG-MB main board (ESP32-S3-WROOM-1-N8R2).

## What it does
- Fan: Off / Sleep / Auto (follows PM2.5) / Turbo, 25 kHz PWM, RPM from FG tach, power from INA226
- Pop-up lift: 0 / 15 / 30 / 46 mm by mode, 5 mm/s silent (TMC2209 StealthChop), anti-pinch via StallGuard, homing at power-up
- Lights: mesh light, running ring, base halo, icon glow, VIAN logo; colours, Steady / Running / Breathing, air-quality colour, night dimming
- Screen: 4.0 in ST7796S, 9 pages, changed with Previous / Next
- Touch icons (AT42QT1070): Power, Previous, Next, Light; Previous + Next 3 s = child lock; Light 5 s = filter reset
- Safety: tilt > 35 deg, front panel removed, no filter -> fan off, cap closes, UV-C off
- Filter life from real dust load and fan-power rise
- Wi-Fi app API: `GET /api/state`, `GET|POST /api/set?mode=auto&color=FFB36B&fx=1&bright=60&aq=0&light=1`; first start opens access point `VIAN-xxxx`, send `ssid`/`pass` to `/api/set` to join home Wi-Fi

## Build
1. Arduino IDE 2.x or arduino-cli, ESP32 core **3.0.x** (Espressif).
2. Libraries: Adafruit NeoPixel, TMCStepper, LovyanGFX, Sensirion Gas Index Algorithm.
3. Board: ESP32S3 Dev Module, Flash 8 MB, PSRAM QSPI, USB CDC On Boot: Enabled.

```
arduino-cli compile --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=8M,PSRAM=enabled VIAN_SlopeGlow
arduino-cli upload  --fqbn esp32:esp32:esp32s3 -p COMx VIAN_SlopeGlow
```

## First hardware bring-up (must do before mass production)
- Confirm the screen module pin-out and backlight polarity.
- The local HTTP API has no login and the OTA/access-point password is shared: add per-unit passwords and authentication before any sale.
- Tune `LIFT_STALL_THRS` so a 2 kg block stops the cap but normal motion does not.
- Replace the dB table and the airflow-per-percent factor with the lab measurements.
- Set a unique Wi-Fi AP password per unit at end-of-line (printed on the rating label).
