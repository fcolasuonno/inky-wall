# inky-wall — Inky Impression 13.3" on self-hosted Tesserae

Battery-first ESP32-S3 firmware for a FireBeetle 2 (N4, no usable PSRAM) driving
a Pimoroni Inky Impression 13.3" Spectra 6 panel as a **Tesserae** MQTT device.
Tesserae renders and hosts the images; this repo is only the device side.

- **One image a day**: the panel wakes, picks up the retained frame URL, paints
  if it changed, sleeps for the server-configured interval (86400 s).
- **Panel Button A** advances to the next image on the spot.
- Nothing talks to Tesserae Cloud: the panel points at a self-hosted Tesserae
  instance, uses your own Mosquitto broker, and gets its Home Assistant
  discovery entities from Tesserae.

## Layout

- `firmware/` — PlatformIO project (`env:tesserae` production, `env:tesserae-bench`
  = same build with deep sleep disabled)

Credentials are not in the repo: `firmware/src/secrets.h` is gitignored, and
`firmware/src/secrets.h.example` is the template to copy.

## Protocol (what the firmware speaks)

| Topic | Dir | Payload |
|---|---|---|
| `tesserae/<id>/frame/bin` | in, retained | `{"url": "http://…/<digest>.bin"}` |
| `tesserae/<id>/config` | in, retained | `{"sleep_interval_s": 86400}` |
| `tesserae/<id>/status` | out, retained | heartbeat: rssi, ip, panel size, `wake_reason`, `last_paint`, `next_sleep_s`, … |
| `tesserae/ha/dev/<id>/cmd/action` | out | `next` (step the lineup), `refresh` (re-render the page) |

The frame is a 4 bpp image, 1200×1600, 600 bytes per row: rows arrive as one
continuous stream and are split on the fly, left 300 bytes to CS0 and right 300
to CS1, so it is downloaded once and never buffered (no PSRAM on this module).

`wake_reason` is `timer` for the daily wake, `button` for a Button A press, and
`poweron`/`usb`/`ext`/`sw`/`panic` for resets. Any non-timer wake asks Tesserae
for the next frame (`next`, then `refresh` after `BUTTON_WAIT_MS`), so the
board's reset button works as a no-solder fallback.

## Wiring (Pi header BCM → FireBeetle GPIO)

| Signal | Pi BCM | FireBeetle GPIO (edit in `config.h`) | FireBeetle silk (3.3 V side strip) |
|---|---|---|---|
| SPI MOSI | 10 | 15 | MO |
| SPI SCLK | 11 | 17 | SCK |
| CS0 (left controller) | 26 | 2 | SCL |
| CS1 (right controller) | 16 | 1 | SDA |
| DC | 22 | 9 | D7 |
| RST | 27 | 18 | D6 |
| BUSY (input, active-LOW) | 17 | 16 | MI |
| 3V3, GND | P1-1, P1-6 | 3V3, GND | 3V3, GND |
| Button A | 5 (P1-29) | 7 | D5 |

Single-strip brought together on the board's 3.3 V edge, from the USB end:
`3V3 · GND · SCK · MO · MI · SCL · SDA · (skip D9) · D7 · D6`. The D9 pad
(GPIO0) is a boot-strapping pin and is left unwired. The button is a separate
wire: header pin 29 → D5, switch closes to GND, RTC pull-up enabled during
sleep. Button C must stay unused — header pin 36 is CS1 in this wiring.

All 3.3 V logic — no level shifting. Avoid ESP32-S3 GPIO 0/3/45/46 (strapping),
19/20 (USB), 43/44 (UART), 33–37 (octal PSRAM).

## Build & flash

```bash
cd firmware
cp src/secrets.h.example src/secrets.h   # then fill in WiFi + MQTT
~/.platformio-venv/bin/pio run -e tesserae
```

Panel-specific settings live in `src/config.h` (pins, button, battery) and
`src/tesserae_config.h` (device id, topics, scan geometry, timing). The device id
must match the `esp32_client` device registered in Tesserae.

Deep sleep removes the USB CDC port, so the board only enumerates while awake:

1. hold **BOOT** while plugging USB (ROM download mode)
2. `~/.platformio-venv/bin/pio run -e tesserae -t upload --upload-port /dev/ttyACM0`
3. plain replug (no BOOT) to run

The post-upload RTS reset does not take on this USB-JTAG board, hence the manual
power cycle. `pio device monitor` refuses piped stdin; use MQTT status or a
pyserial capture with `dtr`/`rts` cleared.

## Runtime & battery

- a timer wake costs a few seconds of radio (unchanged URL = heartbeat only, no
  HTTP, no panel refresh); a button press adds ~31 s of panel refresh
- the wake cadence comes from Tesserae (`sleep_interval_s`, currently a day),
  with 15/30/60 min retry backoff if a download or paint fails
- battery telemetry is off (`BATTERY_ADC_PIN -1`): the board has no sense net,
  so a 100k/100k divider from BAT+ to A0 (GPIO4) would be needed first; see
  `config.h`

## Credits & license

- The panel driver is a C++ port of the EL133UF1 path in
  [pimoroni/inky](https://github.com/pimoroni/inky) (MIT).
- Server side rendering/scheduling is handled by a self-hosted Tesserae
  instance; this repo contains no server code.

MIT — see [LICENSE](LICENSE).
