#pragma once

// Local Tesserae bridge for the paperlesspaper OpenPaper L (epd13).
// The panel itself stays on the original GxEPD2 path; this only replaces the
// "brain" (AWS MQTT / vendor cloud) with a small local MQTT client.

#include "secrets.h"

#define TESSERAE_LOCAL 1

#define TESS_FW_VERSION "paperless-L-tesserae-0.1"

// Timeouts (ms). Same shape as inky-wall/firmware.
#define TESS_WIFI_TIMEOUT_MS   25000UL
#define TESS_MQTT_TIMEOUT_MS   8000UL
#define TESS_RETAINED_WAIT_MS  6000UL
#define TESS_SLEEP_WAIT_MS     15000UL
#define TESS_HTTP_TIMEOUT_MS   30000UL

#define TESS_MIN_SLEEP_S       60UL
#define TESS_MAX_SLEEP_S       (7UL * 24UL * 3600UL)
#define TESS_DEFAULT_SLEEP_S   (8UL * 3600UL)
#define TESS_OTA_WINDOW_MS     (180000UL)  // 3 minutes; ArduinoOTA reboots on success

// Tesserae renders a 1200x1600 portrait 4bpp frame (960000 bytes, 600 B/row).
#define TESS_PANEL_W 1200
#define TESS_PANEL_H 1600
#define TESS_FRAME_BYTES ((size_t)(TESS_PANEL_W * TESS_PANEL_H / 2))
// 4x NiMH pack: empty at BAT_OFF_VALUE (4000 mV), full at ~5600 mV.
#define TESS_BAT_FULL_MV 5600
