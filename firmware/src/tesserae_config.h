#pragma once

// Tesserae MQTT client settings for the Inky Impression 13.3" on the
// FireBeetle 2 ESP32-S3. WiFi / broker credentials and panel wiring live in
// config.h; this file only adds the things the Tesserae device contract needs.

#include "config.h"

// ---- device identity ---------------------------------------------------------
// Must match the device registered in Tesserae (Settings -> Devices).
#define TESSERAE_DEVICE_ID      "inky13"

#define TOPIC_FRAME             "tesserae/" TESSERAE_DEVICE_ID "/frame/bin"
#define TOPIC_CONFIG            "tesserae/" TESSERAE_DEVICE_ID "/config"
#define TOPIC_STATUS            "tesserae/" TESSERAE_DEVICE_ID "/status"

// One-shot server directive: how long to sleep after this wake. Lets the
// server pick the next wake from a wall-clock schedule (the board has no
// RTC clock). Published retained by the wake_schedule plugin on each
// heartbeat and cleared by the firmware after it is consumed.
#define TOPIC_CMD_SLEEP         "tesserae/" TESSERAE_DEVICE_ID "/cmd/sleep"
#define CMD_SLEEP_WAIT_MS       30000UL

// One-shot OTA trigger, published retained by the Tesserae admin plugin.
// The command is consumed (cleared) on the first wake that sees it.
#define TOPIC_CMD_OTA           "tesserae/" TESSERAE_DEVICE_ID "/cmd/ota"
// OTA hostname and one-shot command token live in secrets.h (see its example).
#define OTA_WINDOW_MS           (180000UL)   // 3 minutes; ArduinoOTA reboots on success
#define OTA_HOLD_MS             (5000UL)     // hold Button A this long at wake to open OTA

#define FW_VERSION              "inky13-tesserae-0.3"

// ---- panel scan geometry -----------------------------------------------------
// Tesserae packs at the panel's firmware-native row stride. The Inky glass is
// 1200x1600 portrait, driven as two 600-pixel-wide controllers, so each row is
// 600 bytes: 300 -> CS0 (left half), 300 -> CS1 (right half).
#define PANEL_SCAN_W            1200
#define PANEL_SCAN_H            1600
#define TESS_FRAME_BYTES             ((size_t)PANEL_SCAN_W * PANEL_SCAN_H / 2)  // 960000
#define FRAME_ROW_BYTES         (PANEL_SCAN_W / 2)                          // 600

// ---- timing ------------------------------------------------------------------
// Fallback only: Tesserae's retained config topic normally supplies the cadence
// (currently 86400 s for this device). 8 h if the server says nothing.
#define DEFAULT_SLEEP_S         (8UL * 3600UL)
#define MIN_SLEEP_S             60UL
#define MAX_SLEEP_S             (7UL * 24UL * 3600UL)

// A failed download/paint must not leave the panel stale for a whole day, but
// retrying forever would cost more battery than one missed refresh. Retry at
// 15 min, 30 min, 60 min, then fall back to the normal cadence.
#define RETRY_BASE_S            900UL
#define RETRY_MAX_LEVELS        3

#define WIFI_TIMEOUT_MS         25000
#define MQTT_TIMEOUT_MS         8000
#define RETAINED_WAIT_MS        6000
// A button/reset wake may beat Tesserae's render: the heartbeat itself can be
// what triggers a smart-sync render, and the new URL lands a moment later. If
// the retained URL still matches the last painted one, keep the MQTT session
// open at most this long and leave as soon as it changes. Timer wakes never
// wait (they carry next_sleep_s, so the server renders ahead of them).
#define BUTTON_FRAME_WAIT_MS    10000
#define HTTP_TIMEOUT_MS         30000

// Persisted across deep sleep (RTC) and across power loss (NVS).
#define NVS_NAMESPACE           "tesserae"
