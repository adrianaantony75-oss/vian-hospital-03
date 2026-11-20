// Sensor drivers for VIAN Slope Glow: PMS5003, SHT40, SGP41, LIS2DH12, INA226, VEML7700 (touch is in display.h)
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <VOCGasIndexAlgorithm.h>
#include <NOxGasIndexAlgorithm.h>
#include "config.h"

static TwoWire &busMain = Wire;    // I2C0
static TwoWire &busSens = Wire1;   // I2C1

static uint8_t sensirionCrc(const uint8_t *d, int n) {  // CRC-8 poly 0x31, init 0xFF
  uint8_t c = 0xFF;
  for (int i = 0; i < n; i++) { c ^= d[i]; for (int b = 0; b < 8; b++) c = (c & 0x80) ? (c << 1) ^ 0x31 : c << 1; }
  return c;
}
static bool i2cWrite8(TwoWire &w, uint8_t a, uint8_t reg, uint8_t v) {
  w.beginTransmission(a); w.write(reg); w.write(v); return w.endTransmission() == 0;
}
static bool i2cWrite16(TwoWire &w, uint8_t a, uint8_t reg, uint16_t v) {
  w.beginTransmission(a); w.write(reg); w.write(v >> 8); w.write(v & 0xFF); return w.endTransmission() == 0;
}
static bool i2cRead(TwoWire &w, uint8_t a, uint8_t reg, uint8_t *buf, uint8_t n) {
  w.beginTransmission(a); w.write(reg); if (w.endTransmission(false) != 0) return false;
  if (w.requestFrom(a, n) != n) return false; for (int i = 0; i < n; i++) buf[i] = w.read(); return true;
}

// ---------- PMS5003 (UART1) ----------
struct Pms { uint16_t pm1 = 0, pm25 = 0, pm10 = 0; uint32_t lastOk = 0; uint8_t buf[32]; uint8_t idx = 0; };
static Pms pms;
static void pmsBegin() { pinMode(PIN_PMS_SET, OUTPUT); digitalWrite(PIN_PMS_SET, HIGH); Serial1.begin(9600, SERIAL_8N1, PIN_PMS_RX, PIN_PMS_TX); }
static void pmsPoll() {
  while (Serial1.available()) {
    uint8_t c = Serial1.read();
    if (pms.idx == 0 && c != 0x42) continue;
    if (pms.idx == 1 && c != 0x4D) { pms.idx = 0; continue; }
    pms.buf[pms.idx++] = c;
    if (pms.idx == 32) {
      pms.idx = 0; uint16_t sum = 0; for (int i = 0; i < 30; i++) sum += pms.buf[i];
      if (sum == ((pms.buf[30] << 8) | pms.buf[31])) {   // atmospheric values, bytes 10..15
        pms.pm1 = (pms.buf[10] << 8) | pms.buf[11]; pms.pm25 = (pms.buf[12] << 8) | pms.buf[13];
        pms.pm10 = (pms.buf[14] << 8) | pms.buf[15]; pms.lastOk = millis();
      }
    }
  }
}

// ---------- SHT40 (I2C1, 0x44) ----------
static bool sht40Read(float &t, float &rh) {
  busSens.beginTransmission(0x44); busSens.write(0xFD); if (busSens.endTransmission() != 0) return false;
  delay(10); uint8_t b[6]; if (busSens.requestFrom((uint8_t)0x44, (uint8_t)6) != 6) return false;
  for (int i = 0; i < 6; i++) b[i] = busSens.read();
  if (sensirionCrc(b, 2) != b[2] || sensirionCrc(b + 3, 2) != b[5]) return false;
  t = -45 + 175.0f * ((b[0] << 8) | b[1]) / 65535.0f;
  rh = constrain(-6 + 125.0f * ((b[3] << 8) | b[4]) / 65535.0f, 0.0f, 100.0f); return true;
}

// ---------- SGP41 (I2C1, 0x59) ----------
static VOCGasIndexAlgorithm vocAlgo; static NOxGasIndexAlgorithm noxAlgo;
static uint32_t sgpStart = 0;
static bool sgp41Read(float t, float rh, int32_t &vocIdx, int32_t &noxIdx) {
  uint16_t rhT = (uint16_t)(rh * 65535 / 100), tT = (uint16_t)((t + 45) * 65535 / 175);
  uint8_t a[2] = {(uint8_t)(rhT >> 8), (uint8_t)rhT}, b[2] = {(uint8_t)(tT >> 8), (uint8_t)tT};
  bool conditioning = millis() - sgpStart < 10000;
  busSens.beginTransmission(0x59);
  busSens.write(0x26); busSens.write(conditioning ? 0x12 : 0x19);  // 0x2612 conditioning, 0x2619 measure raw
  busSens.write(a, 2); busSens.write(sensirionCrc(a, 2)); busSens.write(b, 2); busSens.write(sensirionCrc(b, 2));
  if (busSens.endTransmission() != 0) return false;
  delay(50);
  uint8_t n = conditioning ? 3 : 6, r[6];
  if (busSens.requestFrom((uint8_t)0x59, n) != n) return false; for (int i = 0; i < n; i++) r[i] = busSens.read();
  uint16_t sVoc = (r[0] << 8) | r[1]; vocIdx = vocAlgo.process(sVoc);
  if (!conditioning) { uint16_t sNox = (r[3] << 8) | r[4]; noxIdx = noxAlgo.process(sNox); }
  return true;
}

// ---------- LIS2DH12 tilt (I2C0, 0x18) ----------
static bool lisBegin() { uint8_t id; if (!i2cRead(busMain, 0x18, 0x0F, &id, 1) || id != 0x33) return false; return i2cWrite8(busMain, 0x18, 0x20, 0x57); }
static float lisTiltDeg() {  // angle between unit vertical and gravity
  uint8_t b[6]; if (!i2cRead(busMain, 0x18, 0x28 | 0x80, b, 6)) return 0;
  float x = (int16_t)(b[0] | b[1] << 8), y = (int16_t)(b[2] | b[3] << 8), z = (int16_t)(b[4] | b[5] << 8);
  float g = sqrtf(x * x + y * y + z * z); if (g < 1) return 0;
  return acosf(fabsf(z) / g) * 57.2958f;   // board mounted flat, Z = up
}

// ---------- INA226 fan power (I2C0, 0x40), shunt 5 mOhm, current LSB 125 uA ----------
static bool inaBegin() { return i2cWrite16(busMain, 0x40, 0x00, 0x4527) && i2cWrite16(busMain, 0x40, 0x05, 8192); }
static float inaWatts() { uint8_t b[2]; if (!i2cRead(busMain, 0x40, 0x03, b, 2)) return -1; return ((b[0] << 8) | b[1]) * 25 * 0.000125f; }

// ---------- VEML7700 ambient light (I2C0, 0x10) ----------
static bool vemlBegin() { busMain.beginTransmission(0x10); busMain.write(0x00); busMain.write(0x00); busMain.write(0x00); return busMain.endTransmission() == 0; }
static float vemlLux() { uint8_t b[2]; busMain.beginTransmission(0x10); busMain.write(0x04); if (busMain.endTransmission(false)) return -1;
  if (busMain.requestFrom((uint8_t)0x10, (uint8_t)2) != 2) return -1; b[0] = busMain.read(); b[1] = busMain.read(); return (b[0] | b[1] << 8) * 0.0576f; }
