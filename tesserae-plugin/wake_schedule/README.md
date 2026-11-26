# Wake schedule — Tesserae admin plugin

Per-device wall-clock wake times for clockless ESP32 panels. The board cannot
wake at 08:30 / 12:00 / 18:00 by itself, so after each heartbeat this plugin
publishes a one-shot retained directive:

```
tesserae/<device>/cmd/sleep   {"sleep_s": 12345}
```

The firmware applies it and clears the topic.

## Flow

Every wake — timer **or button** — publishes a heartbeat, which is the board
asking Tesserae "when should I wake again?". Tesserae has the clock, so the
plugin computes the seconds to the next configured HH:MM and answers over MQTT:

- timer wake at 12:00 → next 18:00 → sleep 6 h
- button tap at 17:00 → next 18:00 → sleep 1 h
- button tap at 17:30 → next 18:00 → sleep 30 min

No fixed "button nap" — button wakes use exactly the same calculation.

## Requires firmware `inky13-tesserae-0.3+` or `paperless-L-tesserae`

The firmware must subscribe to `tesserae/<id>/cmd/sleep`, parse `sleep_s`,
apply it, and clear the retained topic. Older firmware ignores the topic and
keeps its fixed cadence.

## Install

```bash
scp -P 45 -r tesserae-plugin/wake_schedule muletto@192.168.1.5:/tmp/ws
ssh -p 45 muletto@192.168.1.5 \
  'sudo cp -a /tmp/ws /docker/tesserae/data/marketplace/wake_schedule && \
   sudo chown -R node_exporter:node_exporter /docker/tesserae/data/marketplace/wake_schedule && \
   rm -rf /tmp/ws && sudo docker restart tesserae'
```

Configure at **Settings → Plugins → Wake schedule**.

## Configuration

Each device has a list of `HH:MM` wake times (server local timezone) and an
enabled flag. Config is stored in `schedule.json` in the plugin's data dir. The
page lets you add, update, toggle, remove and force-send a directive.

## Notes

- Duplicates (retained status replay, repeated deliveries) are ignored within a
  60-second window, configurable via the `dedupe_s` plugin setting.
- The directive is published 0.3 s after the heartbeat so the firmware is still
  in its short wait window, and cleared after 60 s if it was never consumed.
- Times strictly in the future are used; if none remain today the first time
  tomorrow is chosen.
- Pair this with `gallery_by_paint` so each wake gets a fresh image.
