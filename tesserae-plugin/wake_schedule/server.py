"""Admin plugin: per-device wall-clock wake schedule for clockless panels.

The ESP32 boards have no RTC, so they cannot wake at 08:30 / 12:00 / 18:00 on
their own. After every heartbeat this plugin computes the seconds to the next
configured wake time and publishes a one-shot retained directive:

    tesserae/<device>/cmd/sleep  {"sleep_s": N}

The firmware applies it and clears the topic. Button wakes use the same
calculation: the board asks Tesserae on every wake, and Tesserae answers with
the seconds to the next configured HH:MM (tap at 17:00 -> 1 h, at 17:30 -> 30 m).

Config lives in schedule.json in the plugin's data dir, so several devices can
have independent wake lists. Admin routes are behind Tesserae's login gate.
"""

from __future__ import annotations

import json
import logging
import re
import threading
import time
from datetime import datetime, timedelta
from pathlib import Path
from typing import Any

from flask import Blueprint, current_app, flash, redirect, render_template, request, url_for

SETTINGS_SECTION = "wake_schedule"
STATE_FILE = "schedule.json"
DEFAULT_TIMES = "08:30,12:00,18:00"
MIN_SLEEP_S = 60
MAX_SLEEP_S = 7 * 24 * 3600
_TIME_RE = re.compile(r"^([01]?\d|2[0-3]):([0-5]\d)$")

logger = logging.getLogger(__name__)

bp = Blueprint("wake_schedule", __name__, template_folder="templates")
_subscribed: set[tuple[int, str]] = set()
_last_seen: dict[tuple[int, str, str], float] = {}


# ----- helpers ---------------------------------------------------------


def _settings() -> dict[str, Any]:
    store = current_app.config.get("SETTINGS_STORE")
    if store is None:
        return {}
    section = store.get_section("plugins") or {}
    return section.get(SETTINGS_SECTION) or {}


def _dedupe_s() -> float:
    try:
        return max(5.0, float(str(_settings().get("dedupe_s") or "60")))
    except ValueError:
        return 60.0


def _own_data_dir() -> Path | None:
    registry = current_app.config.get("PLUGIN_REGISTRY")
    plugin = registry.get("wake_schedule") if registry is not None else None
    return Path(plugin.data_dir) if plugin is not None else None


def _load() -> dict[str, Any]:
    data_dir = _own_data_dir()
    if data_dir is None:
        return {"devices": {}}
    try:
        raw = json.loads((data_dir / STATE_FILE).read_text(encoding="utf-8"))
        devices = raw.get("devices") if isinstance(raw, dict) else None
        return {"devices": devices if isinstance(devices, dict) else {}}
    except (OSError, ValueError):
        return {"devices": {}}


def _save(config: dict[str, Any]) -> None:
    data_dir = _own_data_dir()
    if data_dir is None:
        return
    data_dir.mkdir(parents=True, exist_ok=True)
    path = data_dir / STATE_FILE
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(config, indent=2, sort_keys=True), encoding="utf-8")
    tmp.replace(path)


def _parse_times(raw: str) -> list[str]:
    out: list[str] = []
    for part in str(raw or "").split(","):
        value = part.strip()
        match = _TIME_RE.fullmatch(value)
        if match is None:
            continue
        normalised = f"{int(match.group(1)):02d}:{int(match.group(2)):02d}"
        if normalised not in out:
            out.append(normalised)
    return sorted(out)


def _entry(device_id: str) -> dict[str, Any] | None:
    entry = _load()["devices"].get(device_id)
    return entry if isinstance(entry, dict) else None


def _seconds_to_next_wake(times: list[str]) -> int | None:
    now = datetime.now()
    candidates: list[int] = []
    for value in times:
        hour, minute = (int(x) for x in value.split(":"))
        for day in (0, 1):
            target = (now + timedelta(days=day)).replace(
                hour=hour, minute=minute, second=0, microsecond=0
            )
            if target > now:
                candidates.append(int((target - now).total_seconds()))
    return min(candidates) if candidates else None


def _sleep_topic(device_id: str) -> str:
    return f"tesserae/{device_id}/cmd/sleep"


def _publish_sleep(app: Any, device_id: str, seconds: int) -> None:
    transport = app.config.get("MQTT_TRANSPORT")
    if transport is None:
        return
    seconds = max(MIN_SLEEP_S, min(seconds, MAX_SLEEP_S))
    topic = _sleep_topic(device_id)
    payload = json.dumps({"sleep_s": seconds}).encode()
    # Let the firmware finish publishing its heartbeat and enter the short
    # directive-wait window before the retained message arrives.
    timer = threading.Timer(0.3, lambda: _safe_publish(transport, topic, payload))
    timer.daemon = True
    timer.start()
    # One-shot: if the panel never consumed it, don't steer a later wake.
    clear = threading.Timer(60.0, lambda: _safe_publish(transport, topic, b""))
    clear.daemon = True
    clear.start()
    logger.info("wake_schedule: %s -> sleep %ds", device_id, seconds)


def _safe_publish(transport: Any, topic: str, payload: bytes) -> None:
    try:
        transport.publish(topic, payload, qos=1, retain=True)
    except Exception:  # noqa: BLE001 - broker may be briefly unavailable
        logger.debug("wake_schedule: publish to %s failed", topic, exc_info=True)


# ----- status subscription --------------------------------------------


def _on_status(app: Any, _topic: str, payload: bytes) -> None:
    device_id = "" if _topic.startswith("tesserae/+/") else _topic.split("/")[1] if "/" in _topic else ""
    try:
        body = json.loads(payload)
    except (ValueError, TypeError):
        return
    if not isinstance(body, dict):
        return
    with app.app_context():
        entry = _entry(device_id)
        if entry is None or not entry.get("enabled", True):
            return
        key = (id(app), device_id)
        now = time.monotonic()
        if now - _last_seen.get(key, 0.0) < _dedupe_s():
            return
        _last_seen[key] = now
        entry["last_seen"] = datetime.now().strftime("%F %T")
        entry["last_wake_reason"] = str(body.get("wake_reason") or "unknown")
        config = _load()
        config["devices"][device_id] = entry
        _save(config)

        # Every wake - timer or button - asks the same question: when is the
        # next configured HH:MM? Tesserae has the clock, the board does not.
        seconds = _seconds_to_next_wake(_parse_times(str(entry.get("times") or "")))
        if seconds is None:
            logger.warning("wake_schedule: no future wake time for %s", device_id)
            return
        entry["next_sleep_s"] = seconds
        entry["last_directive_at"] = datetime.now().strftime("%F %T")
        config["devices"][device_id] = entry
        _save(config)
        _publish_sleep(app, device_id, seconds)


def _subscribe_now(app: Any) -> None:
    key = (id(app), "*")
    if key in _subscribed:
        return
    transport = app.config.get("MQTT_TRANSPORT")
    if transport is None:
        return
    transport.subscribe("tesserae/+/status", lambda t, p: _on_status(app, t, p))
    _subscribed.add(key)
    # The broker replays the retained status on subscribe; don't act on it.
    for device_id in _load()["devices"]:
        _last_seen[(id(app), device_id)] = time.monotonic()
    logger.info("wake_schedule: subscribed to tesserae/+/status")


@bp.before_app_request
def _ensure_subscription() -> None:
    try:
        _subscribe_now(current_app._get_current_object())
    except Exception:  # noqa: BLE001 - never break a request over telemetry
        logger.exception("wake_schedule: could not subscribe to status")


def _start_subscription_watcher(app: Any) -> None:
    def watch() -> None:
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            try:
                with app.app_context():
                    if app.config.get("MQTT_TRANSPORT") is not None:
                        _subscribe_now(app)
                        return
            except Exception:  # noqa: BLE001 - retry while the app boots
                logger.debug("wake_schedule: subscription watcher retry", exc_info=True)
            time.sleep(1)

    threading.Thread(target=watch, name="wake-schedule-subscribe", daemon=True).start()


@bp.record_once
def _on_registered(state: Any) -> None:
    _start_subscription_watcher(state.app)


# ----- admin pages -----------------------------------------------------


def _view() -> dict[str, Any]:
    devices = []
    for device_id, entry in sorted(_load()["devices"].items()):
        times = _parse_times(str(entry.get("times") or ""))
        devices.append(
            {
                "id": device_id,
                "times": entry.get("times") or "",
                "enabled": entry.get("enabled", True),
                "next_wake": _seconds_to_next_wake(times),
                "last_seen": entry.get("last_seen"),
                "last_wake_reason": entry.get("last_wake_reason"),
                "next_sleep_s": entry.get("next_sleep_s"),
            }
        )
    return {"devices": devices}


@bp.get("/")
def index() -> str:
    return render_template("wake_schedule/index.html", **_view())


@bp.post("/add")
def add():
    device_id = (request.form.get("device_id") or "").strip()
    times = _parse_times(request.form.get("times") or "")
    if not device_id or not re.fullmatch(r"[A-Za-z0-9_.-]{1,64}", device_id):
        flash("Device id must be letters, digits, '.', '-' or '_'.", "warn")
    elif not times:
        flash("Enter at least one valid HH:MM wake time.", "warn")
    else:
        config = _load()
        config["devices"][device_id] = {
            "times": ",".join(times),
            "enabled": True,
        }
        _save(config)
        flash(f"Added wake schedule for {device_id}: {', '.join(times)}.", "ok")
    return redirect(url_for("wake_schedule.index"))


@bp.post("/update")
def update():
    device_id = (request.form.get("device_id") or "").strip()
    config = _load()
    entry = config["devices"].get(device_id)
    if not isinstance(entry, dict):
        flash(f"No schedule for {device_id}.", "warn")
    else:
        times = _parse_times(request.form.get("times") or "")
        if not times:
            flash("Enter at least one valid HH:MM wake time.", "warn")
        else:
            entry["times"] = ",".join(times)
            entry.pop("button_sleep_s", None)
            _save(config)
            flash(f"Updated {device_id}: {', '.join(times)}.", "ok")
    return redirect(url_for("wake_schedule.index"))


@bp.post("/toggle")
def toggle():
    device_id = (request.form.get("device_id") or "").strip()
    config = _load()
    entry = config["devices"].get(device_id)
    if isinstance(entry, dict):
        entry["enabled"] = not entry.get("enabled", True)
        _save(config)
        flash(f"{device_id}: {'enabled' if entry['enabled'] else 'disabled'}.", "ok")
    return redirect(url_for("wake_schedule.index"))


@bp.post("/remove")
def remove():
    device_id = (request.form.get("device_id") or "").strip()
    config = _load()
    if device_id in config["devices"]:
        del config["devices"][device_id]
        _save(config)
        flash(f"Removed {device_id}.", "ok")
    return redirect(url_for("wake_schedule.index"))


@bp.post("/send")
def send():
    device_id = (request.form.get("device_id") or "").strip()
    entry = _entry(device_id)
    if entry is None:
        flash(f"No schedule for {device_id}.", "warn")
    else:
        seconds = _seconds_to_next_wake(_parse_times(str(entry.get("times") or "")))
        if seconds is None:
            flash("No future wake time configured.", "warn")
        else:
            _publish_sleep(current_app._get_current_object(), device_id, seconds)
            flash(f"Sleep directive sent: {seconds}s.", "ok")
    return redirect(url_for("wake_schedule.index"))


def blueprint() -> Blueprint:
    return bp
