"""Admin plugin: one-shot retained MQTT command that opens the ESP32's
ArduinoOTA window.

The panel is asleep almost all the time, so an OTA window cannot be opened
from the server alone. This plugin publishes a retained command; when the
user next presses Button A, the firmware wakes, reads the command, clears it,
and stays awake with ArduinoOTA for a bounded window.

The admin page is behind Tesserae's normal login/CSRF protection. The command
token is a guard against accidental triggers; the real security boundary is
ArduinoOTA's own password.
"""

from __future__ import annotations

from typing import Any

from flask import Blueprint, current_app, flash, redirect, render_template

DEFAULT_DEVICE_ID = "inky13"
DEFAULT_TOKEN = "inky13-ota-enable"
SETTINGS_SECTION = "inky_ota"


def _settings() -> dict[str, Any]:
    store = current_app.config.get("SETTINGS_STORE")
    if store is None:
        return {}
    section = store.get_section("plugins") or {}
    return section.get(SETTINGS_SECTION) or {}


def _topic() -> str:
    device_id = str(_settings().get("device_id") or DEFAULT_DEVICE_ID).strip()
    return f"tesserae/{device_id}/cmd/ota"


def _token() -> str:
    settings = _settings()
    value = settings.get("token_secret") or settings.get("token") or DEFAULT_TOKEN
    return str(value).strip()


def _publish(payload: bytes) -> str | None:
    """Return an error string, or None on success.

    MQTT_TRANSPORT is an internal Tesserae config key; guard its absence so a
    broker-less install or a future rename fails with a clear message rather
    than a traceback.
    """
    transport = current_app.config.get("MQTT_TRANSPORT")
    if transport is None:
        return "MQTT broker is not configured in Tesserae (no MQTT_TRANSPORT)."
    try:
        transport.publish(_topic(), payload, qos=1, retain=True)
    except Exception as exc:  # noqa: BLE001 - surface any transport error in the UI
        return f"MQTT publish failed: {exc}"
    return None


def blueprint() -> Blueprint:
    bp = Blueprint("inky_ota", __name__, template_folder="templates")

    @bp.get("/")
    def index() -> str:
        return render_template("inky_ota/index.html", topic=_topic())

    @bp.post("/enable")
    def enable():
        error = _publish(_token().encode("utf-8"))
        if error:
            flash(error, "error")
        else:
            flash(f"OTA command published to {_topic()} — now press the panel button.", "ok")
        return redirect("/plugins/inky_ota/")

    @bp.post("/clear")
    def clear():
        error = _publish(b"")
        if error:
            flash(error, "error")
        else:
            flash(f"Retained command cleared on {_topic()}.", "ok")
        return redirect("/plugins/inky_ota/")

    return bp
