// HT1622 segment LCD driver (Holtek) for the VIAN hidden display.
// Display: custom VA negative-mode LCD (black when off, segments pass the RGBW backlight).
// Matrix: 32 SEG x 8 COM. RAM: 64 nibbles; address 2n = SEG n COM0-3, 2n+1 = SEG n COM4-7.
// Command codes follow the HT1621/HT1622 family. Confirm against the HT1622 datasheet
// revision used by the LCD maker before first power-up.
#pragma once
#include <Arduino.h>
#include "config.h"

class HT1622 {
 public:
  void begin() {
    pinMode(PIN_HT_CS, OUTPUT); pinMode(PIN_HT_WR, OUTPUT); pinMode(PIN_HT_DATA, OUTPUT);
    digitalWrite(PIN_HT_CS, HIGH); digitalWrite(PIN_HT_WR, HIGH);
    cmd(0x01);  // SYS EN
    cmd(0x18);  // RC 32K internal oscillator
    cmd(0xE3);  // NORMAL mode
    cmd(0x03);  // LCD ON
    clear(); flush();
  }
  void on(bool v) { cmd(v ? 0x03 : 0x02); }
  void clear() { memset(ram, 0, sizeof(ram)); }
  // set one segment: seg 0..31, com 0..7
  void set(uint8_t seg, uint8_t com, bool v) {
    uint8_t &b = ram[seg]; if (v) b |= (1 << com); else b &= ~(1 << com);
  }
  void flush() {  // write all 32 SEG bytes as 64 nibbles from address 0
    digitalWrite(PIN_HT_CS, LOW);
    bits(0b101, 3);           // WRITE mode id
    bits(0, 6);               // start address 0
    for (int s = 0; s < 32; s++) { bits(ram[s] & 0x0F, 4, true); bits(ram[s] >> 4, 4, true); }
    digitalWrite(PIN_HT_CS, HIGH);
  }
 private:
  uint8_t ram[32];
  void bits(uint16_t v, uint8_t n, bool lsbFirst = false) {
    for (uint8_t i = 0; i < n; i++) {
      bool b = lsbFirst ? (v >> i) & 1 : (v >> (n - 1 - i)) & 1;
      digitalWrite(PIN_HT_WR, LOW); digitalWrite(PIN_HT_DATA, b); delayMicroseconds(2);
      digitalWrite(PIN_HT_WR, HIGH); delayMicroseconds(2);
    }
  }
  void cmd(uint8_t c) {
    digitalWrite(PIN_HT_CS, LOW);
    bits(0b100, 3); bits((uint16_t)c << 1, 9);  // COMMAND mode id + 8-bit command + don't-care bit
    digitalWrite(PIN_HT_CS, HIGH);
  }
};

// ---------- VIAN segment map (matches segment_map.csv) ----------
// 7-segment digits: digit k uses SEG k, COM0..6 = a,b,c,d,e,f,g. COM7 of SEG0-9 unused.
enum : uint8_t { D_AQI0 = 0, D_AQI1, D_AQI2, D_PM0, D_PM1, D_PM2, D_RH0, D_RH1, D_DB0, D_DB1 };
// AQI dial ring: 20 arc segments on SEG10 (COM0-7), SEG11 (COM0-7), SEG12 (COM0-3)
// Icons on SEG13 / SEG14
enum Icon : uint8_t {
  I_AUTO, I_SLEEP, I_TURBO, I_WIFI, I_FILTER, I_FILTER_WARN, I_GOOD, I_MODERATE,   // SEG13 COM0-7
  I_POOR, I_LBL_AQI, I_LBL_PM, I_LBL_UGM3, I_LBL_HUM, I_LBL_PCT, I_LBL_DB, I_LOCK   // SEG14 COM0-7
};
static const uint8_t SEVEN[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

inline void lcdDigit(HT1622 &d, uint8_t pos, int v) {  // v < 0 = blank
  uint8_t m = (v >= 0 && v <= 9) ? SEVEN[v] : 0;
  for (uint8_t c = 0; c < 7; c++) d.set(pos, c, m & (1 << c));
}
inline void lcdNumber(HT1622 &d, uint8_t first, uint8_t n, int value) {  // right aligned, no leading zeros
  for (int i = n - 1; i >= 0; i--) {
    bool lead = (value == 0 && i != n - 1);
    lcdDigit(d, first + i, lead ? -1 : value % 10); value /= 10;
  }
}
inline void lcdIcon(HT1622 &d, Icon i, bool v) { d.set(13 + i / 8, i % 8, v); }
inline void lcdDial(HT1622 &d, uint8_t lit) {  // 0..20 arc segments
  for (uint8_t k = 0; k < 20; k++) d.set(10 + k / 8, k % 8, k < lit);
}
