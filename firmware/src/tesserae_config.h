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
// Tesserae's HA command surface: "next" steps the lineup, "refresh" forces a
// re-render of the current page. Used by the panel button / reset press.
#define TOPIC_CMD_ACTION        "tesserae/ha/dev/" TESSERAE_DEVICE_ID "/cmd/action"

#define FW_VERSION              "inky13-tesserae-0.1"

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
// A button wake waits this long for the server to publish the stepped frame.
// Only paid on a button/reset press, never on the daily timer wake. A refresh
// render takes ~1-3 s, so 12 s covers it with margin while keeping a no-op
// "next" from burning the full window before the refresh fallback.
#define BUTTON_WAIT_MS          12000
#define HTTP_TIMEOUT_MS         30000

// Persisted across deep sleep (RTC) and across power loss (NVS).
#define NVS_NAMESPACE           "tesserae"
