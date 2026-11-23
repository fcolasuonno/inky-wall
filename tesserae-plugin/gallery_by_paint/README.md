# Gallery by paint — Tesserae admin plugin

Pushes one `picture_gallery` image per **confirmed panel paint**. It does **not**
render the gallery dashboard through Chromium — that path intermittently hit a
5-second image wait and blanked the panel. Instead it uses
`PushManager.push_image()`, the same direct-bytes path as **Send → File**, so
every frame is a real image.

## Requires firmware `inky13-tesserae-0.2+`

The heartbeat must carry:

```json
"paint_seq": 42, "painted_digest": "abc123def4567890"
```

- `paint_seq` increments on each successful refresh (persisted across sleep).
- `painted_digest` is the `<digest>` from the frame URL the panel downloaded.

## Install

```bash
scp -P 45 -r tesserae-plugin/gallery_by_paint muletto@192.168.1.5:/tmp/gbp
ssh -p 45 muletto@192.168.1.5 \
  'sudo cp -a /tmp/gbp /docker/tesserae/data/marketplace/gallery_by_paint && \
   sudo chown -R node_exporter:node_exporter /docker/tesserae/data/marketplace/gallery_by_paint && \
   rm -rf /tmp/gbp && sudo docker restart tesserae'
```

## Required server setup

1. **Disable the gallery deck** (set `"enabled": false` for `gallery_at_07_00`
   in `data/core/decks.json`) so scheduled Chromium page renders stop
   overwriting the direct image frames.
2. Plugin settings: `device_id` (`inky13`), `folder` (`wallpapers`).

## Behaviour

1. On the first heartbeat after install/restart, the plugin pushes image 0.
2. The panel paints it on wake and reports `paint_seq` / `painted_digest` in its
   **next** heartbeat (the heartbeat is published before the paint).
3. The plugin checks the event log for a `sent` push with source
   `gallery_by_paint` and that exact digest. On a match, that photo was really
   displayed, so the next image is pushed immediately — ready for the next wake.
4. Button wakes with no new frame don't increment `paint_seq`, so they cost
   nothing. Pushes from other sources don't match and are ignored.

State lives in `state.json` in the plugin's data dir. The admin page shows the
current image, position, last sequence/digest and Push next / Push again /
Reset to first controls.

## Notes

- Images are read via the gallery plugin's public `resolve_image_path()`, so
  external/read-only folders are rejected safely.
- The gallery cell's `file` option is kept in sync for anyone who still opens
  the gallery dashboard manually, but that page is no longer in the push path.
- The event log lookup covers the last 200 pushes; if a paint can't be
  attributed, the plugin waits for the next confirmed one rather than guessing.
