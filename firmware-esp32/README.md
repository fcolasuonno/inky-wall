# inky-wall — classic ESP32 port

This is a port of [`../firmware/`](../firmware/) (FireBeetle 2 ESP32-S3) to a
**classic ESP32** board: ESP32-D0WD-V3, 4 MB flash, CP2102 USB-UART, onboard
18650 holder. Everything else — the Tesserae device id, topics, protocol,
heartbeat, OTA window and panel driver — is unchanged, so the same Tesserae
`esp32_client` device can run either build.

## What changed from the S3 build

| | S3 (`firmware/`) | This port |
|---|---|---|
| Board | `dfrobot_firebeetle2_esp32s3` | `esp32dev` |
| Console | USB CDC (`ARDUINO_USB_*` flags) | Plain UART0 over CP2102 — no USB flags |
| Flash / memory | `qio_qspi` | default DIO, `default.csv` (keeps two OTA partitions) |
| Firmware id | `inky13-tesserae-0.3` | `inky13-tesserae-0.3-esp32` |
| Wake button | GPIO7 (S3 RTC pin) | **GPIO32** (classic ESP32 RTC GPIO) |
| Panel pins | 15/17/2/1/9/18/16 | 23/18/13/14/27/26/25 (see `config.h`) |

Classic ESP32 constraints that drove the pin choices:

- GPIO 6–11 are the internal SPI flash bus — never use them.
- GPIO 0/2/4/5/12/15 are strapping pins; GPIO 1/3 are the CP2102 UART.
- GPIO 34–39 are input-only; GPIO32 is RTC-capable, so it works as the ext1
  deep-sleep wake source for Button A.
- No battery sense exists on this board (all ADC1 pins read 0 V with a charged
  cell). `BATTERY_ADC_PIN -1` stays off; an external 100k/100k divider to GPIO34
  is documented in `config.h` if you add one.

## Wiring (Pi header BCM -> ESP32 GPIO)

| Signal | Pi BCM | ESP32 GPIO |
|---|---|---|
| SPI MOSI | 10 | 23 |
| SPI SCLK | 11 | 18 |
| CS0 (left controller) | 26 | 13 |
| CS1 (right controller) | 16 | 14 |
| DC | 22 | 27 |
| RST | 27 | 26 |
| BUSY (input, active-LOW) | 17 | 25 |
| Button A | 5 (P1-29) | 32 (switch to GND) |
| 3V3, GND | P1-1, P1-6 | 3V3, GND |

All 3.3 V logic — no level shifting.

## Build & flash

```bash
cd firmware-esp32
cp src/secrets.h.example src/secrets.h   # then fill in WiFi + MQTT
~/.platformio-venv/bin/pio run -e tesserae
~/.platformio-venv/bin/pio run -e tesserae -t upload --upload-port /dev/ttyUSB0
```

Unlike the S3's USB-JTAG port, the CP2102 comes back after the post-upload RTS
reset, so no manual power cycle is needed. `pio device monitor` is optional;
plain `Serial` output is on the same UART.

The bench variant (`-e tesserae-bench`) never deep-sleeps.

## OTA flashing

Same contract as the S3 build: hold Button A for 5 s at wake, or publish the
retained command `inky13-ota-enable` to `tesserae/inky13/cmd/ota` and press the
button briefly. The device then holds an ArduinoOTA window for 3 minutes.

```bash
INKY_OTA_PASSWORD=<password> ~/.platformio-venv/bin/pio run -e tesserae-ota -t upload
```

Override `upload_port` in `platformio.ini` with the panel's LAN IP if
`inky13-ota.local` does not resolve.
