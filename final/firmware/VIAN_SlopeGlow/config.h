// VIAN Slope Glow - firmware configuration
// Target: ESP32-S3-WROOM-1-N8R2 on main board VSG-MB rev A
// Toolchain: Arduino-ESP32 core 3.x (ESP-IDF 5.x). Board: "ESP32S3 Dev Module", USB CDC On Boot: Enabled.
// Libraries: Adafruit NeoPixel >= 1.12, TMCStepper >= 0.7.3,
//            Sensirion Gas Index Algorithm >= 3.2 (VOCGasIndexAlgorithm / NOxGasIndexAlgorithm).
#pragma once
#include <Arduino.h>

#define FW_VERSION "VSG-1.0.0"

// ---------- ESP32-S3 pin map (must match VSG-MB netlist / ESP32_pinout.csv) ----------
// I2C0: main board + display board (LIS2DH12 0x18, INA226 0x40, VEML7700 0x10, AT42QT1070 0x1B)
#define PIN_I2C0_SDA   8
#define PIN_I2C0_SCL   9
// I2C1: sensor board on intake (SHT40 0x44, SGP41 0x59)
#define PIN_I2C1_SDA   17
#define PIN_I2C1_SCL   18
// PMS5003 particle sensor, UART1, 9600 8N1
#define PIN_PMS_RX     15   // ESP RX  <- PMS TX
#define PIN_PMS_TX     16   // ESP TX  -> PMS RX
#define PIN_PMS_SET    14   // high = run, low = sleep
// EC fan: 25 kHz PWM via open-drain 2N7002 (inverted: MOSFET on = PWM line low), FG tach 2 pulses/rev
#define PIN_FAN_PWM    4
#define PIN_FAN_TACH   5
// Pop-up lift: TMC2209 (single-wire UART on PDN_UART through 1k), micro linear stepper
#define PIN_TMC_TX     10
#define PIN_TMC_RX     11
#define PIN_LIFT_STEP  12
#define PIN_LIFT_DIR   13
#define PIN_LIFT_EN    21   // low = driver enabled
#define PIN_LIFT_DIAG  1    // StallGuard output, high = stall
#define PIN_LIFT_HOME  2    // bottom home switch, low = closed (cap fully down)
// Addressable LEDs (SK6812 RGBW, 5 V) through 74AHCT125 level shifter
#define PIN_LED_GROOVE 38   // CH1 groove light line, 96 LEDs
#define PIN_LED_RING   39   // CH2 pop-up running ring, 84 LEDs
#define PIN_LED_BASE   40   // CH3 base halo, 36 LEDs
#define PIN_LED_FRONT  41   // CH4 VIAN logo, 12 LEDs (through the display board)
// Outside display: 4.0" IPS ST7796S on SPI2 + FT6336U touch on I2C0 (0x38). LCD/touch reset = ESP_EN.
#define PIN_LCD_CS     42
#define PIN_LCD_SCK    47
#define PIN_LCD_MOSI   48
#define PIN_LCD_DC     45   // strapping pin, 10 k pull-down on the main board
#define PIN_LCD_BL     43   // backlight PWM
#define PIN_TP_INT     6    // touch interrupt, low = touch
// Safety inputs
#define PIN_DOOR       35   // reed switch on front intake panel, low = panel closed
#define PIN_FILTER     36   // DRV5032 hall sensor, low = magnet in filter frame present
#define PIN_TILT_INT   37   // LIS2DH12 INT1 (not required, tilt also polled)
// Outputs
#define PIN_BUZZER     7    // passive buzzer via 2N7002, 2.7 kHz
#define PIN_UV_RELAY   3    // UV-C relay (Hospital / Pro only). Hardware door interlock in series.

// ---------- LED counts ----------
#define N_GROOVE 96
#define N_RING   84
#define N_BASE   36
#define N_LOGO   12
#define N_FRONT  N_LOGO
// Global LED current cap: 5 V LED rail is 3 A. Firmware limits average brightness.
#define LED_MAX_BRIGHT 64   // of 255, about 25 % -> worst case ~2.5 A all channels white

// ---------- Lift mechanics ----------
// Lead screw 2 mm/rev, 200 full steps/rev, 16 microsteps -> 1600 steps per mm
#define LIFT_STEPS_PER_MM 1600L
#define LIFT_MAX_MM       46
#define LIFT_SPEED_MM_S   5.0f      // slow and quiet
#define LIFT_RUN_CURRENT  350       // mA RMS
#define LIFT_STALL_THRS   60        // TMC2209 SGTHRS, tune on first build
#define LIFT_SETTLE_MS    30000UL   // only move after fan mode has been stable this long (Auto)
#define LIFT_H_SLEEP 15
#define LIFT_H_AUTO  30
#define LIFT_H_TURBO 46

// ---------- Fan ----------
#define FAN_PWM_HZ    25000
#define FAN_PWM_BITS  10
#define FAN_MIN_PCT   18     // below this the EC fan stops
#define FAN_SLEEP_PCT 35     // 578 rpm, 262 m3/h, ~29 dB(A)
#define FAN_TURBO_PCT 100
// Quiet behaviour
#define FAN_RAMP_PCT_PER_S 4      // speed changes slowly: no sudden roar
#define FAN_NIGHT_MAX_PCT  45     // Auto is capped at night (Turbo by hand still allowed)
static const uint8_t FAN_SKIP_BANDS[][2] = {{0, 0}};  // resonance bands to skip, e.g. {{52, 56}}; set from the end-of-line sweep

// ---------- Safety ----------
#define TILT_LIMIT_DEG 35.0f

// ---------- Filters (3 stages) ----------
#define HEPA_CAPACITY_G   920.0f   // LifeHEPA LF-5: 23 m2 x 40 g/m2 (confirm with lab dust-loading test)
#define HEPA_MAX_HOURS    21900.0f // 5 years x 12 h/day hygiene limit
#define FILTER_POWER_RISE 0.25f    // +25 % fan power at Turbo -> HEPA end of life
#define ODOUR_MAX_HOURS   4380.0f  // OdourCore OC-12: 12 months x 12 h/day
#define ODOUR_VOC_DOSE    2.0e6f   // VOC-index-above-100 x seconds that saturates the carbon (lab value)
#define DEBRIS_WASH_DAYS  21       // DebrisGuard rinse reminder

// ---------- Wi-Fi ----------
#define AP_PREFIX "VIAN-"
#define AP_PASS   "vianair123"    // change per unit at end-of-line (printed on label)
