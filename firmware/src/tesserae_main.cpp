// Tesserae MQTT client for the Inky Impression 13.3" / FireBeetle 2 ESP32-S3.
//
// One wake = one MQTT session:
//   connect -> receive retained frame URL + retained config -> heartbeat ->
//   (only if the URL changed) download the .bin once and split-stream it to the
//   two panel controllers -> deep sleep for the server-configured interval.
//
// Battery decisions:
//   * no second MQTT/WiFi session after the paint (heartbeat is sent before it;
//     next_sleep_s is what the server's scheduler actually needs)
//   * no NTP (no clock, no sync radio time)
//   * no Home Assistant discovery from the device (Tesserae owns it)
//   * unchanged frame URL = no HTTP request and no 20 s panel refresh
//   * one download per paint: the 600-byte rows are de-interleaved on the fly
//     (this board has no PSRAM, so a framebuffer is not an option)
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <ctype.h>

#include "config.h"
#include "tesserae_config.h"
#include "inky_el133.h"

#define RTC_MAGIC 0x54455353u   // "TESS"

RTC_DATA_ATTR uint32_t rtcMagic = 0;
RTC_DATA_ATTR uint32_t bootCount = 0;
RTC_DATA_ATTR uint32_t sleepS = 0;
RTC_DATA_ATTR uint8_t paintFails = 0;
RTC_DATA_ATTR char lastUrl[200] = "";
RTC_DATA_ATTR char lastPaint[8] = "";
RTC_DATA_ATTR uint32_t lastPaintMs = 0;

static WiFiClient net;
static PubSubClient mqtt(net);
static InkyEL133 panel;
static Preferences prefs;

// sampled once at boot, before WiFi/panel current draw skews the rail
static uint32_t battMv = 0;
static int battPct = -1;

static volatile bool gotFrame = false;
static volatile bool gotConfig = false;
static char frameUrl[200] = "";
static uint32_t cfgSleep = 0;

// ---- small JSON helpers (payloads are not NUL-terminated) --------------------
static bool jsonString(const char* data, size_t len, const char* key,
                       char* out, size_t outSz) {
    char needle[40];
    int nlen = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (nlen <= 0 || nlen >= (int)sizeof(needle)) return false;

    const char* p = nullptr;
    for (size_t i = 0; i + (size_t)nlen <= len; i++) {
        if (memcmp(data + i, needle, (size_t)nlen) == 0) { p = data + i; break; }
    }
    if (!p) return false;
    p += nlen;
    const char* end = data + len;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end || *p != ':') return false;
    p++;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end || *p != '"') return false;
    p++;
    const char* q = (const char*)memchr(p, '"', (size_t)(end - p));
    if (!q) return false;
    size_t vlen = (size_t)(q - p);
    if (vlen >= outSz) vlen = outSz - 1;
    memcpy(out, p, vlen);
    out[vlen] = '\0';
    return vlen > 0;
}

static bool jsonInt(const char* data, size_t len, const char* key, int32_t* out) {
    char needle[40];
    int nlen = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (nlen <= 0 || nlen >= (int)sizeof(needle)) return false;

    const char* p = nullptr;
    for (size_t i = 0; i + (size_t)nlen <= len; i++) {
        if (memcmp(data + i, needle, (size_t)nlen) == 0) { p = data + i; break; }
    }
    if (!p) return false;
    p += nlen;
    const char* end = data + len;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end || *p != ':') return false;
    p++;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end || !isdigit((unsigned char)*p)) return false;
    int32_t v = 0;
    while (p < end && isdigit((unsigned char)*p)) {
        v = v * 10 + (*p - '0');
        p++;
    }
    *out = v;
    return true;
}

// ---- state -------------------------------------------------------------------

static void loadState() {
    if (rtcMagic == RTC_MAGIC) return;   // deep-sleep wake: RTC already has it

    prefs.begin(NVS_NAMESPACE, true);
    String url = prefs.getString("url", "");
    sleepS = prefs.getULong("sleep_s", 0);
    prefs.end();

    strncpy(lastUrl, url.c_str(), sizeof(lastUrl) - 1);
    lastUrl[sizeof(lastUrl) - 1] = '\0';
    lastPaint[0] = '\0';
    lastPaintMs = 0;
    rtcMagic = RTC_MAGIC;
}

static void saveUrl(const char* url) {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putString("url", url);
    prefs.end();
}

static void saveSleep(uint32_t s) {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putULong("sleep_s", s);
    prefs.end();
}

// ---- connectivity ------------------------------------------------------------

static bool connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t0 > WIFI_TIMEOUT_MS) return false;
        delay(200);
    }
    Serial.printf("[wifi] %s  RSSI %d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
}

static void mqttCallback(char* topic, byte* payload, unsigned int len) {
    if (strcmp(topic, TOPIC_FRAME) == 0) {
        if (jsonString((const char*)payload, len, "url", frameUrl, sizeof(frameUrl))) {
            gotFrame = true;
            Serial.printf("[mqtt] frame url: %s\n", frameUrl);
        }
    } else if (strcmp(topic, TOPIC_CONFIG) == 0) {
        int32_t v = 0;
        if (jsonInt((const char*)payload, len, "sleep_interval_s", &v)) {
            cfgSleep = (uint32_t)v;
            gotConfig = true;
            Serial.printf("[mqtt] config sleep_interval_s=%ld\n", (long)v);
        }
    }
}

static bool connectMQTT() {
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setSocketTimeout(5);
    mqtt.setKeepAlive(60);
    mqtt.setBufferSize(768);
    mqtt.setCallback(mqttCallback);

    String id = String(TESSERAE_DEVICE_ID) + "-" + String((uint32_t)esp_random(), HEX);
    bool ok;
    if (MQTT_USER[0] != '\0') {
        ok = mqtt.connect(id.c_str(), MQTT_USER, MQTT_PASS);
    } else {
        ok = mqtt.connect(id.c_str());
    }
    if (!ok) {
        Serial.printf("[mqtt] connect failed rc=%d\n", mqtt.state());
        return false;
    }
    mqtt.subscribe(TOPIC_FRAME, 1);
    mqtt.subscribe(TOPIC_CONFIG, 1);
    return true;
}

// retained topics arrive immediately after SUBACK
static void waitRetained() {
    uint32_t t0 = millis();
    while (!gotFrame && mqtt.connected() && millis() - t0 < RETAINED_WAIT_MS) {
        mqtt.loop();
        delay(2);
    }
    // give a retained config a moment too, but never block the wake on it
    t0 = millis();
    while (!gotConfig && mqtt.connected() && millis() - t0 < 1500) {
        mqtt.loop();
        delay(2);
    }
}

// ---- button / reset press ----------------------------------------------------

// Panel Button A (GPIO7, see PIN_BUTTON) is armed as an ext1 wake source, so a
// press wakes the chip like a reset does. Any wake that is not the deep-sleep
// timer therefore counts as a press: the panel asks Tesserae to move on and
// repaints whatever comes back on the same wake. Tesserae's HA command topic is
// the only "next" surface a pure MQTT device has (the REST button path in the
// esp32_client contract needs a device token and an extra HTTP session).
static bool isButtonWake() {
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) return true;
    return esp_reset_reason() != ESP_RST_DEEPSLEEP;
}

static void publishAction(const char* action) {
    if (!mqtt.connected()) return;
    if (mqtt.publish(TOPIC_CMD_ACTION, action, false)) {
        Serial.printf("[cmd] %s\n", action);
    } else {
        Serial.printf("[cmd] %s failed\n", action);
    }
}

// Polls the retained frame topic until the URL differs from `baseUrl`.
static bool waitNewFrame(const char* baseUrl, uint32_t timeoutMs) {
    uint32_t t0 = millis();
    while (mqtt.connected() && millis() - t0 < timeoutMs) {
        mqtt.loop();
        if (gotFrame && frameUrl[0] && strcmp(frameUrl, baseUrl) != 0) {
            Serial.printf("[cmd] new frame after %lums\n",
                          (unsigned long)(millis() - t0));
            return true;
        }
        delay(5);
    }
    return false;
}

// One step of whatever the display follows (lineup / rotation). A single-dashboard
// lineup has nowhere to step to, so fall back to a forced re-render, which is
// what advances a sequential Gallery widget to the next photo.
static bool advanceFrame() {
    char baseUrl[sizeof(frameUrl)];
    strncpy(baseUrl, frameUrl, sizeof(baseUrl));
    baseUrl[sizeof(baseUrl) - 1] = '\0';

    publishAction("next");
    if (waitNewFrame(baseUrl, BUTTON_WAIT_MS)) return true;

    publishAction("refresh");
    if (waitNewFrame(baseUrl, BUTTON_WAIT_MS)) return true;

    Serial.println("[cmd] no new frame (is HA discovery enabled?)");
    return false;
}


// ---- heartbeat ---------------------------------------------------------------

static const char* wakeReason() {
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) return "button";
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "ext";
        case ESP_RST_SW:        return "sw";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:       return "watchdog";
        case ESP_RST_DEEPSLEEP: return "timer";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_USB:       return "usb";
        default:                return "unknown";
    }
}

static uint32_t computeNextSleep() {
    uint32_t s = sleepS;
    if (s < MIN_SLEEP_S || s > MAX_SLEEP_S) s = DEFAULT_SLEEP_S;
    if (paintFails > 0 && paintFails <= RETRY_MAX_LEVELS) {
        uint32_t retry = RETRY_BASE_S << (paintFails - 1);   // 15m, 30m, 60m
        if (retry < s) s = retry;
    }
    return s;
}

static void publishHeartbeat(uint32_t nextSleep) {
    char body[420];
    int n = snprintf(body, sizeof(body), "{");

    if (battPct >= 0) {
        n += snprintf(body + n, sizeof(body) - (size_t)n,
                      "\"battery_mv\":%u,\"battery_pct\":%d,",
                      (unsigned)battMv, battPct);
    }

    n += snprintf(body + n, sizeof(body) - (size_t)n,
                  "\"rssi\":%d,\"ip\":\"%s\",\"fw_version\":\"%s\","
                  "\"kind\":\"esp32_client\",\"panel_w\":%d,\"panel_h\":%d,"
                  "\"sleep_interval_s\":%lu,\"next_sleep_s\":%lu,"
                  "\"wake_reason\":\"%s\"",
                  WiFi.RSSI(), WiFi.localIP().toString().c_str(), FW_VERSION,
                  PANEL_SCAN_W, PANEL_SCAN_H,
                  (unsigned long)sleepS, (unsigned long)nextSleep, wakeReason());

    if (lastPaint[0] && n > 0 && (size_t)n < sizeof(body)) {
        n += snprintf(body + n, sizeof(body) - (size_t)n,
                      ",\"last_paint\":\"%s\",\"last_paint_ms\":%lu",
                      lastPaint, (unsigned long)lastPaintMs);
    }
    if (paintFails > 0 && n > 0 && (size_t)n < sizeof(body)) {
        n += snprintf(body + n, sizeof(body) - (size_t)n,
                      ",\"paint_failures\":%u", (unsigned)paintFails);
    }
    if (n > 0 && (size_t)n < sizeof(body) - 2) {
        body[n++] = '}';
        body[n] = '\0';
    }

    mqtt.publish(TOPIC_STATUS, body, true);
    Serial.printf("[mqtt] status: %s\n", body);
    delay(100);   // let QoS0 + DISCONNECT leave the socket before teardown
}

static void sampleBattery() {
#if BATTERY_ADC_PIN >= 0
    analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += analogReadMilliVolts(BATTERY_ADC_PIN);
        delay(2);
    }
    battMv = (uint32_t)((float)(sum / 16) * BATTERY_DIVIDER);
    battPct = (int)(((float)battMv - 3300.0f) / (4200.0f - 3300.0f) * 100.0f);
    if (battPct < 0) battPct = 0;
    if (battPct > 100) battPct = 100;
    Serial.printf("[batt] %umV -> %d%%\n", (unsigned)battMv, battPct);
#endif
}

// ---- frame fetch + paint -----------------------------------------------------

static bool parseHttpUrl(const char* url, char* host, size_t hostSz,
                         uint16_t* port, char* path, size_t pathSz) {
    if (strncmp(url, "http://", 7) != 0) return false;
    const char* p = url + 7;
    const char* slash = strchr(p, '/');
    if (!slash) return false;
    const char* colon = (const char*)memchr(p, ':', (size_t)(slash - p));

    size_t hlen = colon ? (size_t)(colon - p) : (size_t)(slash - p);
    if (hlen == 0 || hlen >= hostSz) return false;
    memcpy(host, p, hlen);
    host[hlen] = '\0';

    *port = 80;
    if (colon) {
        long v = strtol(colon + 1, nullptr, 10);
        if (v <= 0 || v > 65535) return false;
        *port = (uint16_t)v;
    }
    strncpy(path, slash, pathSz - 1);
    path[pathSz - 1] = '\0';
    return true;
}

static bool fetchAndPaint(const char* url) {
    char host[64];
    char path[160];
    uint16_t port = 80;
    if (!parseHttpUrl(url, host, sizeof(host), &port, path, sizeof(path))) {
        Serial.printf("[http] unusable url: %s\n", url);
        return false;
    }

    WiFiClient client;
    if (!client.connect(host, port, HTTP_TIMEOUT_MS)) {
        Serial.println("[http] connect failed");
        return false;
    }
    client.printf("GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);

    String headers;
    headers.reserve(512);
    uint32_t t0 = millis();
    bool headerDone = false;
    while (client.connected() || client.available()) {
        if (client.available()) {
            String line = client.readStringUntil('\n');
            headers += line + "\n";
            t0 = millis();
            if (line == "\r") { headerDone = true; break; }
        } else if (millis() - t0 > HTTP_TIMEOUT_MS) {
            break;
        } else {
            delay(2);
        }
    }
    if (!headerDone) {
        Serial.println("[http] header read failed");
        client.stop();
        return false;
    }
    int idx = headers.indexOf("Content-Length:");
    if (idx < 0) {
        Serial.println("[http] no Content-Length");
        client.stop();
        return false;
    }
    size_t contentLength = (size_t)headers.substring(idx + 15).toInt();
    if (contentLength != TESS_FRAME_BYTES) {
        Serial.printf("[http] bad length %u (want %u)\n",
                      (unsigned)contentLength, (unsigned)TESS_FRAME_BYTES);
        client.stop();
        return false;
    }
    if (headers.indexOf(" 200 ") < 0) {
        Serial.println("[http] non-200 response");
        client.stop();
        return false;
    }

    Serial.printf("[http] streaming %u bytes, one pass, split rows\n",
                  (unsigned)contentLength);
    uint32_t paintStart = millis();

    panel.setFrameSpeed(InkyEL133::SPEED_NORMAL);
    panel.begin();
    panel.powerUp();
    bool ok = panel.streamSplit(client, contentLength, HTTP_TIMEOUT_MS);
    client.stop();

    if (ok) {
        panel.commit();
        lastPaintMs = millis() - paintStart;
        strncpy(lastPaint, "ok", sizeof(lastPaint));
        Serial.printf("[inky] painted in %lums\n", (unsigned long)lastPaintMs);
    } else {
        // no PON/DRF was sent, so the glass keeps the previous image
        strncpy(lastPaint, "failed", sizeof(lastPaint));
        lastPaintMs = millis() - paintStart;
        Serial.println("[inky] stream failed - old image kept");
    }
    return ok;
}

// ---- sleep -------------------------------------------------------------------

static void goToSleep() {
#ifdef DEBUG_NO_SLEEP
    Serial.println("[main][DEBUG] sleep disabled, idling");
    while (true) {
        delay(10000);
        Serial.printf("[main] alive %lus, heap %u, RSSI %d\n",
                      (unsigned long)(millis() / 1000),
                      (unsigned)ESP.getFreeHeap(), WiFi.RSSI());
    }
#endif
    uint32_t sleepNow = computeNextSleep();
    Serial.printf("[main] deep sleep %lus (paint failures %u)\n",
                  (unsigned long)sleepNow, (unsigned)paintFails);
    Serial.flush();
    delay(50);

#if PIN_BUTTON >= 0
    // Panel button A: a level wake source, so it costs nothing while idle (the
    // pull-up only draws when the switch closes). Wait for release first, or a
    // finger still on the button would wake the chip straight back up.
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    uint32_t tRel = millis();
    while (digitalRead(PIN_BUTTON) == LOW && millis() - tRel < 3000) delay(20);
    if (digitalRead(PIN_BUTTON) == LOW) {
        Serial.println("[btn] still held - button wake skipped for this cycle");
    } else {
        rtc_gpio_pullup_en((gpio_num_t)PIN_BUTTON);
        rtc_gpio_pulldown_dis((gpio_num_t)PIN_BUTTON);
        esp_sleep_enable_ext1_wakeup(1ULL << PIN_BUTTON, ESP_EXT1_WAKEUP_ANY_LOW);
        Serial.println("[btn] armed");
    }
#endif

    esp_sleep_enable_timer_wakeup((uint64_t)sleepNow * 1000000ULL);
    esp_deep_sleep_start();
}

// ---- main --------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(250);
    uint32_t tWait = millis();
    while (!Serial && millis() - tWait < 3000) { delay(10); }

    bool cold = (rtcMagic != RTC_MAGIC);
    loadState();
    bootCount++;
    Serial.printf("\n[main] boot #%lu (%s) fw=%s\n",
                  (unsigned long)bootCount, cold ? "cold" : "timer", FW_VERSION);
    Serial.printf("[main] last url '%s' last paint '%s'\n", lastUrl, lastPaint);
    sampleBattery();

    if (!connectWiFi()) {
        Serial.println("[wifi] failed");
        goToSleep();
    }
    if (!connectMQTT()) {
        Serial.println("[mqtt] failed");
        goToSleep();
    }

    waitRetained();
    if (gotConfig && cfgSleep >= MIN_SLEEP_S && cfgSleep <= MAX_SLEEP_S &&
        cfgSleep != sleepS) {
        sleepS = cfgSleep;
        saveSleep(sleepS);
        Serial.printf("[main] cadence -> %lus\n", (unsigned long)sleepS);
    }
    if (sleepS == 0) sleepS = DEFAULT_SLEEP_S;

    // exactly one heartbeat per wake, before any long panel work. next_sleep_s
    // is the pre-paint estimate: on a failed paint the device wakes *earlier*
    // than advertised (retry backoff), which is the safe direction.
    publishHeartbeat(computeNextSleep());

    // Button A / reset: ask for the next frame on this same wake, while the MQTT
    // session is still up, so the paint below uses the stepped URL.
    if (isButtonWake()) {
        Serial.println("[main] button press -> advance");
        if (gotFrame && frameUrl[0]) advanceFrame();
    }
    mqtt.disconnect();

    bool paint = gotFrame && frameUrl[0] && strcmp(frameUrl, lastUrl) != 0;

    if (!gotFrame || frameUrl[0] == 0) {
        Serial.println("[main] no frame published yet");
    } else if (!paint) {
        Serial.println("[main] frame unchanged, skipping paint");
        paintFails = 0;
    } else if (fetchAndPaint(frameUrl)) {
        strncpy(lastUrl, frameUrl, sizeof(lastUrl) - 1);
        lastUrl[sizeof(lastUrl) - 1] = '\0';
        saveUrl(lastUrl);
        paintFails = 0;
    } else {
        if (paintFails < RETRY_MAX_LEVELS) paintFails++;
        Serial.printf("[main] paint failed (%u), retry sooner\n", (unsigned)paintFails);
    }

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    goToSleep();
}

void loop() {}
