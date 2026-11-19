# Inky13 OTA trigger (Tesserae admin plugin)

Publishes a one-shot retained MQTT command that opens the panel's ArduinoOTA
window. The panel sleeps for hours, so the window cannot be opened from the
server alone: you publish the command, then press the panel button. (The
firmware also opens the same window on a 5-second long press of Button A, so
the plugin is optional.)

## Install

Copy this folder into the Tesserae server's `plugins/` directory and restart
Tesserae:

```bash
cp -r tesserae-plugin/inky_ota /path/to/tesserae/plugins/
```

It appears under **Settings → Plugins → Inky13 OTA trigger**.

## Use

1. Click **Enable OTA window** on that page (or publish the command yourself:
   `mosquitto_pub -t tesserae/inky13/cmd/ota -m inky13-ota-enable -r`).
2. Press the panel button. The firmware wakes, clears the retained command and
   keeps WiFi awake for 3 minutes with ArduinoOTA.
3. Upload:
   `INKY_OTA_PASSWORD=<password> pio run -e tesserae-ota -t upload`

## Configuration

The admin page exposes `device_id` (default `inky13`) and `token` (default
`inky13-ota-enable`). The token must match `OTA_CMD_TOKEN` in the firmware's
`tesserae_config.h`. The token only guards against accidental triggers; the
real security boundary is ArduinoOTA's password (`OTA_PASSWORD` in
`src/secrets.h`).

## Notes

- Uses the internal `MQTT_TRANSPORT` Tesserae config key; it is guarded with
  `.get()` so a broker-less install or a future rename fails with a clear
  message instead of a traceback. Pin your Tesserae version if you rely on it.
- If your Tesserae version's admin template differs, adjust
  `templates/inky_ota/index.html` (it extends `_base.html`, block `main`).
