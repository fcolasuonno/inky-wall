firmware-paperless-L-tesserae
=============================
OpenPaper L (epd13) firmware that uses a LOCAL Tesserae instance instead of the
paperlesspaper AWS cloud. Based on paperlesspaper-firmware-offline, not on the
cloud firmware. No DEBUG_NO_SLEEP: normal deep sleep is used.

What changed
------------
- Added knolleary/PubSubClient.
- Added src/tesserae_config.h + src/secrets.h(.example) with local
  MQTT broker host/port/user/pass.
- Added a small MQTT bridge in src/main.cpp guarded by TESSERAE_LOCAL:
    * subscribes tesserae/<CLIENT_ID>/frame/bin, /config, /cmd/sleep
    * publishes tesserae/<CLIENT_ID>/status heartbeat
    * downloads the retained .bin frame into the existing external-flash slot
      tmp.bmp and displays it with the existing GxEPD2 path.
- Tesserae .bin frames are already in hardware LUT format, so
  setImageFromFS_13inch() skips getColor() when the bridge sets
  tessRawLutFrame. The panel driver, pins and partition table are unchanged.
- The offline firmware's BLE setup (Wi-Fi credentials, etc.) is retained and
  is opened by the big reset/button wake. Compiled WIFI_SSID / WIFI_PASS from
  secrets.h are used as a fallback when no SSID has been configured over BLE.
- Accelerometer wake (motionWakeup) is enabled by default and forced on in the
  Tesserae bridge. The big reset/button wake still opens the short BLE setup
  window before doing a Tesserae update.
- The status heartbeat now includes battery_mv and battery_pct (4x NiMH:
  4000 mV empty, 5600 mV full). Voltage is sampled early in setup, before
  WiFi / panel current draw.
- cmd/ota support: retained topic tesserae/<id>/cmd/ota with payload
  TESS_OTA_CMD_TOKEN opens a 180 s ArduinoOTA window (password protected).
  The command is consumed by publishing an empty retained payload. Successful
  upload reboots into the new image. No button-hold trigger is implemented.

Configure
---------
1. cp src/secrets.h.example src/secrets.h
2. Edit WIFI_SSID / WIFI_PASS, MQTT_HOST / PORT / USER / PASS for your local
   broker, and OTA_HOSTNAME / OTA_PASSWORD / TESS_OTA_CMD_TOKEN for ArduinoOTA.
   The secrets file now uses the same layout as inky-wall/firmware/src/secrets.h.
3. Register the device in Tesserae with ID equal to CLIENT_ID, which is the
   BLE-advertised name, e.g. epd13-1020BAFFFE F739E0. Topics are:
     tesserae/<id>/frame/bin   in,  retained  {"url":"http://.../<digest>.bin"}
     tesserae/<id>/config      in,  retained  {"sleep_interval_s":N}
     tesserae/<id>/cmd/sleep   in,  retained  {"sleep_s":N}  (one-shot)
     tesserae/<id>/status      out, retained  heartbeat with paint_seq,
                                               painted_digest, wake_reason, ...
4. pio run -e epd13
5. Remote OTA upload (while an OTA window is open): set INKY_OTA_PASSWORD to
   OTA_PASSWORD, then run:
     INKY_OTA_PASSWORD='your-ota-password' pio run -e tesserae-ota -t upload
   Override upload_port in platformio.ini if mDNS does not resolve.

Flashing - Option A (app0, easy rollback)
-----------------------------------------
The current firmware boots from app0 at 0x10000. The full 4 MB backup made on
2026-11-26 is in /home/fra/paperlesspaper-backup/. Extract the original app0
image once:

  dd if=/home/fra/paperlesspaper-backup/full_flash_epd13-1020baf739e0_2026-11-26.bin \
     of=/home/fra/paperlesspaper-backup/app0_original_epd13.bin \
     bs=4096 skip=16 count=1856

Put the frame into ROM download mode: hold the small BOOT button, briefly tap
the big RESET button, release BOOT.

Flash the new firmware to app0:

  esptool --port /dev/ttyUSB0 --baud 460800 write_flash 0x10000 \
     /home/fra/inky-wall/firmware-paperless-L-tesserae/.pio/build/epd13/firmware.bin

To go back to the original cloud firmware:

  esptool --port /dev/ttyUSB0 --baud 460800 write_flash 0x10000 \
     /home/fra/paperlesspaper-backup/app0_original_epd13.bin

Ultimate fallback - restore the complete 4 MB image:

  esptool --port /dev/ttyUSB0 --baud 460800 write_flash 0x0 \
     /home/fra/paperlesspaper-backup/full_flash_epd13-1020baf739e0_2026-11-26.bin

SPIFFS and the partition table are not touched by the app0 flash above.

Wake behaviour
--------------
- Accelerometer wake: enabled by default (motionWakeup=true) and forced on by
  the Tesserae bridge, so GPIO0 is armed as an ext1 wake source at every sleep.
  A motion wake skips BLE setup and goes straight to a Tesserae update.
- Big reset/button wake: still works. It opens the BLE setup window for a
  short time (unless skipSetupMode), then proceeds to a Tesserae update.
- Small BOOT button: unchanged; hold it during reset for ROM download mode.
- Timer wake: normal scheduled Tesserae poll/sleep.
