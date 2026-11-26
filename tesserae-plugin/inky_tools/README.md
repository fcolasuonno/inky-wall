# Inky tools — Tesserae admin plugin

One admin page with two tabs, merged from the former `inky_ota` and
`gallery_import` plugins:

- **Gallery import** — save a picture into a `picture_gallery` folder from a
  pasted URL or a clipboard/dropped image. The stock Gallery page only has a
  file picker.
- **OTA trigger** — publish the one-shot retained MQTT command that opens the
  ESP32's ArduinoOTA window on its next button wake.

`gallery_by_paint` stays a separate plugin: it is a background service with
its own state and page-mutation lifecycle.

## Install

Persistent plugin dir (not `data/plugins/`, which is per-plugin data storage):

```bash
scp -P 45 -r tesserae-plugin/inky_tools muletto@192.168.1.5:/tmp/inky_tools
ssh -p 45 muletto@192.168.1.5 \
  'sudo cp -a /tmp/inky_tools /docker/tesserae/data/marketplace/inky_tools && \
   sudo chown -R node_exporter:node_exporter /docker/tesserae/data/marketplace/inky_tools && \
   sudo rm -rf /tmp/inky_tools && sudo docker restart tesserae'
```

Then remove the two old folders and restart again:

```bash
ssh -p 45 muletto@192.168.1.5 \
  'sudo rm -rf /docker/tesserae/data/marketplace/inky_ota \
              /docker/tesserae/data/marketplace/gallery_import && \
   sudo docker restart tesserae'
```

Settings from the old sections (`inky_ota`, `gallery_import`) are read as
fallbacks, so existing values survive the move. New values go to the single
`inky_tools` section under **Settings → Plugins → Inky tools**.

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `device_id` | `inky13` | Topic prefix for the OTA command. |
| `default_folder` | first folder | Folder pre-selected on the import tab. |
| `max_size_mb` | `20` | Cap on fetched / pasted bytes (1–100). |
| `allowed_hosts` | empty | Comma-separated hostnames allowed for URL imports. |
| `token` | `inky13-ota-enable` | Must match `TESS_OTA_CMD_TOKEN` in the firmware's `src/secrets.h`. |

## Import tab

Pick an internal folder (or type a name to create one), then either paste an
image URL and click **Save from URL**, or click the paste box, press **Ctrl+V**
(or drop an image) and click **Save pasted image**. Files are written through
`picture_gallery`'s public storage helpers, so external folders stay read-only
and duplicates get `-1`, `-2`, …. The extension comes from the image bytes.
Supported: `.jpg`, `.png`, `.webp`, `.gif`, `.bmp`.

## OTA tab

Click **Enable OTA window**, then press the panel button. The firmware wakes,
clears the retained command, and holds ArduinoOTA for 3 minutes. Upload with:

```bash
INKY_OTA_PASSWORD=<password> pio run -e tesserae-ota -t upload
```

## Security notes

- Admin routes sit behind Tesserae's login and provenance/CSRF gate.
- URL fetches are http(s) only; credential URLs and loopback, link-local,
  metadata (`169.254.169.254`), multicast, reserved and unspecified addresses
  are rejected. Redirects are re-checked. Private LAN addresses are allowed so
  you can import from e.g. a Home Assistant camera; narrow with `allowed_hosts`.
  This is a best-effort SSRF guard, not a sandbox — the manifest declares
  `network:*` for transparency.
- Image bytes are sniffed and Pillow-verified before being written.
