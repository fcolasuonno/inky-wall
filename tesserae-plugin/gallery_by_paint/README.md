# Gallery by paint — Tesserae admin plugin

Pins one `picture_gallery` photo per **confirmed panel paint**. Stock
`sequential` mode advances the album on every render, so a sleeping panel
"consumes" 12 photos between wakes. This plugin bypasses that cursor: it owns
the cell's `file` pin and moves it only when the panel reports it actually
painted a frame pushed from the gallery page.

## Requires firmware `inky13-tesserae-0.2+`

The heartbeat must carry:

```json
"paint_seq": 42, "painted_digest": "abc123def4567890"
```

- `paint_seq` increments on each successful refresh (persisted across sleep).
- `painted_digest` is the `<digest>` from the frame URL the panel downloaded.

The firmware side is in this repo: `firmware/src/tesserae_main.cpp` and
`tesserae_config.h` (`FW_VERSION`). Flash it before enabling the plugin.

## Install

Copy into the Tesserae server's persistent plugin dir (not `data/plugins/`,
which is per-plugin data storage):

```bash
scp -P 45 -r tesserae-plugin/gallery_by_paint \
  muletto@192.168.1.5:/tmp/gbp
ssh -p 45 muletto@192.168.1.5 \
  'sudo cp -a /tmp/gbp /docker/tesserae/data/marketplace/gallery_by_paint && \
   sudo chown -R node_exporter:node_exporter /docker/tesserae/data/marketplace/gallery_by_paint && \
   rm -rf /tmp/gbp && sudo docker restart tesserae'
```

It appears under **Settings → Plugins → Gallery by paint**.

## Settings

| Setting | Default | Meaning |
|---|---|---|
| `device_id` | `inky13` | Panel whose heartbeats drive the lineup. |
| `page_id` | gallery page id | Dashboard containing the `picture_gallery` cell. |
| `folder` | `wallpapers` | Internal gallery folder to rotate. |

## Behaviour

1. On first heartbeat the plugin seeds `paint_seq` and pins image 0 — no step
   is consumed for the frame the panel is already showing.
2. Tesserae pushes the gallery page; the panel paints it on wake.
3. The **next** heartbeat (sent before the next paint, by firmware design)
   reports the digest of the frame it just displayed.
4. The plugin checks the event log for a `sent` push of the gallery page whose
   frame digest matches. If it matches, that photo was consumed and the pin
   moves to the next image.
5. Button wakes with no new frame do not increment `paint_seq`, so they cost
   nothing. Pushes from the Send tab, History resend of other pages, or
   another dashboard target a different page and are ignored.

State is stored in `state.json` in the plugin's data dir; the admin page shows
the current pin, position, last sequence/digest and Skip/Reset controls.

## Notes

- The gallery cell should stay in `mode: random` — the `file` pin takes
  precedence in `fetch()`, so the widget's own cursor is never touched and no
  random choice happens.
- The subscription is registered lazily on the first HTTP request via
  `MQTT_TRANSPORT.subscribe()`; Tesserae's own loopback renders guarantee that
  happens shortly after a restart.
- If the event log has been pruned, a paint can't be attributed and the plugin
  waits for the next confirmed one rather than guessing.
- After a firmware reflash that resets NVS, `paint_seq` restarts at 0; the
  plugin notices the decrease and re-seeds without advancing.
