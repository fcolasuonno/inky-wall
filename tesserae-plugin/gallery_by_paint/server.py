"""Admin plugin: one gallery photo per *confirmed panel paint*, pushed directly.

The Chromium page-render path is unreliable for a full-screen photo: its
5-second image wait intermittently captures a blank frame. This plugin avoids
Chromium entirely — it pushes the gallery image bytes straight through the
panel renderer with ``PushManager.push_image()``, the same path the Send tab
uses for a file or URL.

Firmware contract (inky13-tesserae-0.2+): the status heartbeat carries

    "paint_seq": <n>, "painted_digest": "<digest>"

``paint_seq`` increments on each successful refresh. ``painted_digest`` is the
<digest> from the frame URL the panel downloaded. The plugin queries the event
log to check that the painted digest was a push from this plugin — an unrelated
Send-tab push lands under a different source and is ignored.

On each confirmed paint the plugin advances its index and pushes the next
image, so the panel has a fresh frame waiting at its next wake. The gallery
deck should be disabled so scheduled page renders cannot overwrite it.

Admin routes are behind Tesserae's normal login/provenance gate.
"""

from __future__ import annotations

import json
import logging
import re
import sqlite3
import threading
import time
from pathlib import Path
from typing import Any

from flask import Blueprint, current_app, flash, redirect, render_template, url_for

from app.state.page_store import page_revision

SETTINGS_SECTION = "gallery_by_paint"
STATE_FILE = "state.json"
DIGEST_RE = re.compile(r"^[0-9a-fA-F]{8,32}$")

logger = logging.getLogger(__name__)

bp = Blueprint("gallery_by_paint", __name__, template_folder="templates")
_subscribed: set[tuple[int, str]] = set()


# ----- helpers ---------------------------------------------------------


def _settings() -> dict[str, Any]:
    store = current_app.config.get("SETTINGS_STORE")
    if store is None:
        return {}
    section = store.get_section("plugins") or {}
    return section.get(SETTINGS_SECTION) or {}


def _text_setting(name: str, default: str) -> str:
    return str(_settings().get(name) or default).strip() or default


def _device_id() -> str:
    return _text_setting("device_id", "inky13")


def _page_id() -> str:
    return _text_setting("page_id", "029ae3f66722")


def _folder() -> str:
    return _text_setting("folder", "wallpapers")


def _own_data_dir() -> Path | None:
    registry = current_app.config.get("PLUGIN_REGISTRY")
    plugin = registry.get("gallery_by_paint") if registry is not None else None
    return Path(plugin.data_dir) if plugin is not None else None


def _state() -> dict[str, Any]:
    data_dir = _own_data_dir()
    if data_dir is None:
        return {}
    path = data_dir / STATE_FILE
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
        return raw if isinstance(raw, dict) else {}
    except (OSError, ValueError):
        return {}


def _save_state(state: dict[str, Any]) -> None:
    data_dir = _own_data_dir()
    if data_dir is None:
        return
    data_dir.mkdir(parents=True, exist_ok=True)
    path = data_dir / STATE_FILE
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(state, indent=2, sort_keys=True), encoding="utf-8")
    tmp.replace(path)


def _gallery_module() -> Any | None:
    registry = current_app.config.get("PLUGIN_REGISTRY")
    plugin = registry.get("picture_gallery") if registry is not None else None
    if plugin is None or plugin.server_module is None:
        return None
    return plugin.server_module


def _images() -> list[str]:
    gallery = _gallery_module()
    if gallery is None:
        return []
    folder = _folder()
    try:
        return sorted(gallery.list_folder_files(folder))
    except Exception:  # noqa: BLE001 - gallery may be an older build
        return []


PUSH_SOURCE = "gallery_by_paint"


def _plugin_push_exists(digest: str) -> bool:
    """True when one of *our* direct image pushes published this frame.

    Anything else that paints the panel (Send tab, a dashboard push) lands
    under a different source, so it can't be mistaken for a consumed photo.
    """
    if not DIGEST_RE.fullmatch(digest):
        return False
    data_root = current_app.config.get("DATA_ROOT")
    if not data_root:
        return False
    db = Path(data_root) / "core" / "events.db"
    if not db.exists():
        return False
    try:
        conn = sqlite3.connect(f"file:{db}?mode=ro", uri=True, timeout=5)
        try:
            rows = conn.execute(
                """SELECT extra_json FROM events
                   WHERE type='push' AND status='sent' AND source=?
                   ORDER BY id DESC LIMIT 200""",
                (PUSH_SOURCE,),
            ).fetchall()
            for (raw,) in rows:
                try:
                    extra = json.loads(raw or "{}")
                except ValueError:
                    continue
                for renderer in extra.get("renderers") or []:
                    if str(renderer.get("digest") or "") == digest:
                        return True
            return False
        finally:
            conn.close()
    except sqlite3.Error as exc:
        logger.warning("gallery_by_paint: event lookup failed: %s", exc)
        return False


def _pin_current(app: Any) -> None:
    """(Re)apply the state's index as the gallery cell's pinned file."""
    with app.app_context():
        images = _images()
        state = _state()
        if not images:
            return
        index = int(state.get("index") or 0) % len(images)
        name = images[index]
        store = current_app.config.get("PAGE_STORE")
        page = store.get(_page_id()) if store is not None else None
        if page is None:
            return
        cell = next((c for c in page.cells if c.plugin == "picture_gallery"), None)
        if cell is None:
            return
        if cell.options.get("file") == name:
            return
        revision = page_revision(page)

        def transform(p: Any) -> Any:
            target = next((c for c in p.cells if c.plugin == "picture_gallery"), None)
            if target is None:
                return p
            target.options["file"] = name
            return p

        store.update_if_revision(_page_id(), revision, transform)
        state["index"] = index
        state["pinned"] = name
        _save_state(state)
        logger.info("gallery_by_paint: pinned %s (index %d/%d)", name, index, len(images))


def _push_photo(app: Any, name: str) -> None:
    """Push one gallery image straight to the panel — no Chromium involved."""
    with app.app_context():
        gallery = _gallery_module()
        if gallery is None:
            raise ValueError("picture_gallery plugin is not installed")
        path = gallery.resolve_image_path(_folder(), name)
        if path is None:
            raise ValueError(f"gallery image '{name}' is missing")
        manager = current_app.config.get("PUSH_MANAGER")
        if manager is None:
            raise ValueError("PUSH_MANAGER is unavailable")
        result = manager.push_image(
            path.read_bytes(),
            source_label=name,
            device_id=_device_id(),
            fit="fill",
            source=PUSH_SOURCE,
        )
        if getattr(result, "status", None) == "failed":
            raise ValueError(f"push failed: {getattr(result, 'error', '?')}")
        logger.info("gallery_by_paint: pushed %s -> %s", name, _device_id())


def _advance(app: Any, state: dict[str, Any]) -> None:
    images = _images()
    if not images:
        logger.warning("gallery_by_paint: no images in folder '%s'", _folder())
        return
    index = (int(state.get("index") or 0) + 1) % len(images)
    name = images[index]
    state["index"] = index
    state["pinned"] = name
    _save_state(state)
    try:
        _push_photo(app, name)
    except Exception as exc:  # noqa: BLE001 - keep the index, report in admin page
        state["last_error"] = str(exc)
        _save_state(state)
        logger.exception("gallery_by_paint: could not push %s", name)
    else:
        state.pop("last_error", None)
        _save_state(state)
    # Keep the cell's pin in sync for anyone who opens the gallery page.
    _pin_current(app)


def _on_status(app: Any, _topic: str, payload: bytes) -> None:
    try:
        body = json.loads(payload)
    except (ValueError, TypeError):
        return
    if not isinstance(body, dict):
        return
    seq = body.get("paint_seq")
    if not isinstance(seq, int) or isinstance(seq, bool):
        return

    with app.app_context():
        state = _state()
        last = state.get("last_seq")
        if last is None:
            # First heartbeat after install/restart: seed and put image 0 in
            # the retained topic so the next wake has something to paint.
            state["last_seq"] = seq
            state["last_digest"] = body.get("painted_digest")
            state.setdefault("index", -1)
            _save_state(state)
            _advance(app, state)
            return
        if seq == last:
            return  # duplicate / button wake with no new paint
        if seq < last:
            # Firmware reflashed / NVS cleared: re-seed, don't count it.
            state["last_seq"] = seq
            state["last_digest"] = body.get("painted_digest")
            _save_state(state)
            return

        state["last_seq"] = seq
        digest = body.get("painted_digest")
        state["last_digest"] = digest
        confirmed = isinstance(digest, str) and _plugin_push_exists(digest)
        state["last_confirmed"] = confirmed
        _save_state(state)

        if confirmed:
            _advance(app, state)
            logger.info("gallery_by_paint: consumed frame %s", digest)


def _subscribe_now(app: Any) -> None:
    device = _device_id()
    key = (id(app), device)
    if key in _subscribed:
        return
    transport = app.config.get("MQTT_TRANSPORT")
    if transport is None:
        return
    topic = f"tesserae/{device}/status"
    transport.subscribe(topic, lambda t, p: _on_status(app, t, p))
    _subscribed.add(key)
    logger.info("gallery_by_paint: subscribed to %s", topic)


@bp.before_app_request
def _ensure_subscription() -> None:
    """Fallback for request-driven startup."""
    try:
        _subscribe_now(current_app._get_current_object())
    except Exception:  # noqa: BLE001 - never break a request over telemetry
        logger.exception("gallery_by_paint: could not subscribe to status")


def _start_subscription_watcher(app: Any) -> None:
    """Register at startup without depending on an HTTP request.

    With the gallery deck disabled there may be no loopback renders at all,
    so a purely lazy subscription would never appear after a restart.
    """

    def watch() -> None:
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            try:
                with app.app_context():
                    if app.config.get("MQTT_TRANSPORT") is not None:
                        _subscribe_now(app)
                        return
            except Exception:  # noqa: BLE001 - retry while the app boots
                logger.debug("gallery_by_paint: subscription watcher retry", exc_info=True)
            time.sleep(1)

    threading.Thread(target=watch, name="gallery-by-paint-subscribe", daemon=True).start()


@bp.record_once
def _on_blueprint_registered(state: Any) -> None:
    _start_subscription_watcher(state.app)


# ----- admin pages -----------------------------------------------------


def _view() -> dict[str, Any]:
    state = _state()
    images = _images()
    index = int(state.get("index") or 0)
    current = state.get("pinned")
    if not current and images:
        current = images[index % len(images)]
    return {
        "state": state,
        "images": images,
        "index": index,
        "current": current,
        "next_image": images[(index + 1) % len(images)] if images else None,
        "device": _device_id(),
        "page_id": _page_id(),
        "folder": _folder(),
    }


@bp.get("/")
def index() -> str:
    return render_template("gallery_by_paint/index.html", **_view())


@bp.post("/skip")
def skip():
    app = current_app._get_current_object()
    _advance(app, _state())
    flash("Pushed the next gallery photo.", "ok")
    return redirect(url_for("gallery_by_paint.index"))


@bp.post("/reset")
def reset():
    app = current_app._get_current_object()
    state = _state()
    state["index"] = -1
    _save_state(state)
    _advance(app, state)
    flash("Reset to the first gallery photo and pushed it.", "ok")
    return redirect(url_for("gallery_by_paint.index"))


@bp.post("/repin")
def repin():
    app = current_app._get_current_object()
    state = _state()
    images = _images()
    if images:
        name = images[int(state.get("index") or 0) % len(images)]
        try:
            _push_photo(app, name)
            flash(f"Pushed {name} again.", "ok")
        except ValueError as exc:
            flash(str(exc), "warn")
    else:
        flash("No gallery images available.", "warn")
    return redirect(url_for("gallery_by_paint.index"))


def blueprint() -> Blueprint:
    return bp
