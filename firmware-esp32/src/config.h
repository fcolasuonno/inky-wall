#pragma once

// ---- credentials -------------------------------------------------------------
// WiFi + MQTT values live in secrets.h, which is gitignored. Copy
// secrets.h.example to secrets.h and fill it in (see README).
#include "secrets.h"

// Tesserae device settings (id, topics, timing) live in tesserae_config.h.

// ---- panel wiring (Pi header BCM -> classic ESP32 GPIO) ----------------------
// Ported from the FireBeetle 2 ESP32-S3 build in ../firmware/.
//
// Classic ESP32 constraints, unlike the S3 build:
//   - GPIO 6..11 are the internal SPI flash bus: never use them.
//   - GPIO 0, 2, 4, 5, 12, 15 are strapping pins: avoid them.
//   - GPIO 1/3 are UART0 (the CP2102 console): avoid them.
//   - GPIO 34..39 are input-only (fine for BUSY / button / ADC).
//   - VSPI defaults: SCK 18, MOSI 23, MISO 19 (MISO unused here).
//
// Use the VSPI pins for the panel and keep the RTC-capable GPIO32 for the
// deep-sleep wake button (see below). All lines are 3.3 V logic.
#define PIN_MOSI            23   // VSPI MOSI
#define PIN_SCK             18   // VSPI SCK
#define PIN_CS0             13   // left controller chip select
#define PIN_CS1             14   // right controller chip select
#define PIN_DC              27   // command/data
#define PIN_RST             26   // panel reset
#define PIN_BUSY            25   // input, active-LOW while the panel is busy

// ---- panel button A (optional) ------------------------------------------------
// Inky Impression Button A sits on physical pin 29 (BCM5). Wire it to GPIO32.
// GPIO32 is an RTC GPIO on the classic ESP32, so it can be armed as an ext1
// deep-sleep wake source; it is also input-only, which is fine for a switch to
// GND with the internal (RTC) pull-up. -1 disables the feature.
//
// Do NOT reuse the panel pins above, and do NOT use GPIO0/2/4/5/12/15
// (strapping), 6-11 (flash), 1/3 (UART), or a non-RTC pin for the wake.
#define PIN_BUTTON          32   // GPIO32, active LOW (switch to GND)

// ---- battery reporting (optional, off by default) ------------------------------
// The 18650 board has no battery-sense net (verified: GPIO34/35/36/39 all read
// 0 V with a charged cell), so one must be added externally:
//   BAT+ -- 100k -- GPIO34 -- 100k -- GND
// Use an ADC1 pin (GPIO32..39); ADC2 is unavailable while WiFi is active.
// A 100 nF cap from GPIO34 to GND is recommended with 100k/100k (50k source Z).
// Tap BAT+ at the battery holder, NOT the 5 V/VIN pin (5 V when USB is plugged).
// -1 skips the "battery"/"battery_pct" fields.
#define BATTERY_ADC_PIN     34   // ADC1_CH6; 100k/100k divider
#define BATTERY_DIVIDER     2.0f // external divider ratio
