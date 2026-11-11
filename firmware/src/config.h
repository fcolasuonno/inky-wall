#pragma once

// ---- credentials -------------------------------------------------------------
// WiFi + MQTT values live in secrets.h, which is gitignored. Copy
// secrets.h.example to secrets.h and fill it in (see README).
#include "secrets.h"

// Tesserae device settings (id, topics, timing) live in tesserae_config.h.

// ---- panel wiring (Pi header BCM -> FireBeetle silk). See README. ------------
// Single-strip plan on the 3V3 side (top->bottom: 3V3 GND SCK MO MI SCL SDA
// D9* D7 D6; *D9/GPIO0 is boot strapping - pad skipped). All strip GPIOs route
// only to the unpopulated GDI FPC connector, so they're free.
// Avoid: GPIO 0/3/45/46 (strapping), 19/20 (USB), 43/44 (UART), 33-37 (PSRAM).
#define PIN_MOSI            15   // silk MO
#define PIN_SCK             17   // silk SCK
#define PIN_CS0             2    // silk SCL
#define PIN_CS1             1    // silk SDA
#define PIN_DC              9    // silk D7
#define PIN_RST             18   // silk D6
#define PIN_BUSY            16   // input, active-LOW while the panel is busy (silk MI)

// ---- panel button A (optional) ------------------------------------------------
// Inky Impression Button A sits on physical pin 29 (BCM5). It is not used by the
// display wiring, so a wire from header pin 29 to the FireBeetle's D5 pad
// (ESP32-S3 GPIO7, RTC-capable, not a strapping pin) turns it into a deep-sleep
// wake source: press -> next image. -1 disables the feature. Do NOT use
// NONE/Button C (header pin 36 is CS1 here) or Button D (BCM24, pin 18).
#define PIN_BUTTON          7    // D5 / GPIO7, active LOW (switch to GND)

// ---- battery reporting (optional, off by default) ------------------------------
// The FireBeetle 2 (N4) has no battery-sense net, so add one externally:
// BAT+ -- 100k -- A0/GPIO4 -- 100k -- GND, then set pin 4 and divider 2.0.
// Tap BAT+ at the battery connector, NOT the 5V pin (5 V when USB is plugged).
// -1 skips the "battery"/"battery_pct" fields.
#define BATTERY_ADC_PIN     -1   // -1 = disabled
#define BATTERY_DIVIDER     2.0f // external divider ratio
