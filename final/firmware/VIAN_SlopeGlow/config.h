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
#define PIN_LED_FRONT  41   // CH4 display backlight (8) then VIAN logo (12) = 20 LEDs
// HT1622 segment LCD driver on display board (3-wire)
#define PIN_HT_CS      42
#define PIN_HT_WR      47
#define PIN_HT_DATA    48
// Touch controller AT42QT1070 change line (open drain, low = key change)
#define PIN_QT_CHANGE  6
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
#define N_DISP_BL 8
#define N_LOGO   12
#define N_FRONT  (N_DISP_BL + N_LOGO)
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
#define FAN_SLEEP_PCT 22
#define FAN_TURBO_PCT 100

// ---------- Safety ----------
#define TILT_LIMIT_DEG 35.0f

// ---------- Filter life ----------
#define FILTER_CAPACITY_G 60.0f   // HC-360 dust holding to end of life (lab value to confirm)
#define FILTER_POWER_RISE 0.25f   // +25 % fan power at same speed -> change filter

// ---------- Wi-Fi ----------
#define AP_PREFIX "VIAN-"
#define AP_PASS   "vianair123"    // change per unit at end-of-line (printed on label)
