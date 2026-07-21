#pragma once

// CrowPanel 1.28" HMI ESP32-S3 Rotary Display
// pins from Elecrow RotaryScreen_1_28_new demo + ESPHome reference config
#define PIN_TP_SDA      6
#define PIN_TP_SCL      7
#define PIN_TP_INT      5
#define PIN_TP_RST      13

#define PIN_ENC_A       45
#define PIN_ENC_B       42
#define PIN_ENC_SW      41  // knob press, active low, pullup

#define PIN_BACKLIGHT   46  // LEDC PWM
#define PIN_POWER_LIGHT 40  // factory demo drives LOW at boot
#define PIN_PWR1        1   // panel/LED power rail, drive HIGH before display init
#define PIN_PWR2        2   // panel/LED power rail, drive HIGH before display init

#define PIN_LED         48  // WS2812 ring
#define LED_COUNT       5

// display SPI (see lgfx_panel.h): SCLK 10, MOSI 11, DC 3, CS 9, RST 14
