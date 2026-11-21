"""Admin plugin for the inky13 panel: gallery import + one-shot OTA trigger.

Two utilities, one settings section and one admin page with tabs:

  * Import — save an image into a picture_gallery folder from a pasted URL
    or a clipboard/dropped image.
  * OTA    — publish a retained MQTT command that opens the panel's
    ArduinoOTA window on its next button wake.

The gallery writes go through picture_gallery's public storage helpers, so
external folders stay read-only and thumbnails/orientation caches behave as
usual. Admin routes are behind Tesserae's normal login + provenance gate.
"""

from __future__ import annotations

import base64
import ipaddress
import re
import socket
import time
from io import BytesIO
from pathlib import Path
from typing import Any
from urllib.parse import unquote, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener

from flask import Blueprint, current_app, flash, redirect, render_template, request, url_for
from werkzeug.utils import secure_filename

DEFAULT_DEVICE_ID = "inky13"
DEFAULT_TOKEN = "inky13-ota-enable"
DEFAULT_MAX_MB = 20
MAX_MB_CEILING = 100
TIMEOUT_S = 15

SETTINGS_SECTION = "inky_tools"
LEGACY_SECTIONS = ("inky_ota", "gallery_import")

MIME_TO_SUFFIX = {
    "image/jpeg": ".jpg",
    "image/jpg": ".jpg",
    "image/png": ".png",
    "image/webp": ".webp",
    "image/gif": ".gif",
    "image/bmp": ".bmp",
    "image/x-ms-bmp": ".bmp",
}

_DATA_URL_RE = re.compile(r"^data:(image/[a-z0-9.+-]+);base64,(.*)$", re.DOTALL | re.IGNORECASE)


# ----- settings (with migration from the two old plugin sections) --------


def _settings() -> dict[str, Any]:
    store = current_app.config.get("SETTINGS_STORE")
    if store is None:
        return {}
    plugins = store.get_section("plugins") or {}
    merged = dict(plugins.get(SETTINGS_SECTION) or {})
    for legacy in LEGACY_SECTIONS:
        for key, value in (plugins.get(legacy) or {}).items():
            merged.setdefault(key, value)
    return merged


def _device_id() -> str:
    return str(_settings().get("device_id") or DEFAULT_DEVICE_ID).strip()


def _ota_topic() -> str:
    return f"tesserae/{_device_id()}/cmd/ota"


def _ota_token() -> str:
    settings = _settings()
    value = settings.get("token_secret") or settings.get("token") or DEFAULT_TOKEN
    return str(value).strip()


def _max_bytes() -> int:
    try:
        mb = int(str(_settings().get("max_size_mb") or DEFAULT_MAX_MB).strip())
    except ValueError:
        mb = DEFAULT_MAX_MB
    return max(1, min(mb, MAX_MB_CEILING)) * 1024 * 1024


def _allowed_hosts() -> set[str]:
    raw = str(_settings().get("allowed_hosts") or "")
    return {h.strip().lower().rstrip(".") for h in raw.split(",") if h.strip()}


# ----- OTA trigger ------------------------------------------------------


def _publish(payload: bytes) -> str | None:
    transport = current_app.config.get("MQTT_TRANSPORT")
    if transport is None:
        return "MQTT broker is not configured in Tesserae (no MQTT_TRANSPORT)."
    try:
        transport.publish(_ota_topic(), payload, qos=1, retain=True)
    except Exception as exc:  # noqa: BLE001 - surface transport errors in the UI
        return f"MQTT publish failed: {exc}"
    return None


# ----- gallery import helpers -------------------------------------------


def _root_value(gallery: Any) -> str:
    return str(getattr(gallery, "ROOT_FOLDER_VALUE", "_root"))


def _gallery_module() -> Any | None:
    registry = current_app.config.get("PLUGIN_REGISTRY")
    plugin = registry.get("picture_gallery") if registry is not None else None
    if plugin is None or plugin.server_module is None:
        return None
    return plugin.server_module


def _sniff_suffix(blob: bytes) -> str | None:
    if blob.startswith(b"\xff\xd8\xff"):
        return ".jpg"
    if blob.startswith(b"\x89PNG\r\n\x1a\n"):
        return ".png"
    if blob.startswith((b"GIF87a", b"GIF89a")):
        return ".gif"
    if blob[:4] == b"RIFF" and blob[8:12] == b"WEBP":
        return ".webp"
    if blob.startswith(b"BM"):
        return ".bmp"
    return None


def _verify_image(blob: bytes) -> None:
    try:
        from PIL import Image
    except ImportError:
        return
    try:
        with Image.open(BytesIO(blob)) as image:
            image.verify()
    except Exception as exc:  # noqa: BLE001 - any decode failure is a bad image
        raise ValueError("That file isn't a valid image.") from exc


def _ip_flag(address: Any, name: str) -> bool:
    """Python 3.13 made ipaddress flags methods; 3.11/3.12 use properties."""
    value = getattr(address, name, False)
    return value() if callable(value) else bool(value)


def _check_url(url: str, allowed_hosts: set[str]) -> str:
    parts = urlsplit(url)
    if parts.scheme not in {"http", "https"} or not parts.hostname:
        raise ValueError("Only http:// and https:// image URLs are supported.")
    if parts.username or parts.password:
        raise ValueError("URLs containing credentials aren't allowed.")
    host = parts.hostname.lower().rstrip(".")
    if allowed_hosts and host not in allowed_hosts:
        raise ValueError(f"Host '{host}' isn't in the plugin's allowed-hosts setting.")
    try:
        port = parts.port or (443 if parts.scheme == "https" else 80)
        infos = socket.getaddrinfo(host, port, type=socket.SOCK_STREAM)
    except (OSError, ValueError) as exc:
        raise ValueError(f"Could not resolve '{host}'.") from exc
    for info in infos:
        try:
            address = ipaddress.ip_address(info[4][0])
        except ValueError:
            continue
        if any(
            _ip_flag(address, name)
            for name in ("is_loopback", "is_link_local", "is_multicast", "is_unspecified", "is_reserved")
        ):
            raise ValueError(f"Refusing to fetch a local/special address for '{host}'.")
    return url


class _CheckedRedirect(HTTPRedirectHandler):
    def __init__(self, allowed_hosts: set[str]) -> None:
        self.allowed_hosts = allowed_hosts

    def redirect_request(self, req, fp, code, msg, headers, newurl):  # type: ignore[no-untyped-def]
        _check_url(newurl, self.allowed_hosts)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def _fetch_url(url: str, max_bytes: int, allowed_hosts: set[str]) -> tuple[bytes, str]:
    _check_url(url, allowed_hosts)
    opener = build_opener(_CheckedRedirect(allowed_hosts))
    request_obj = Request(
        url,
        headers={"User-Agent": "Tesserae InkyTools/1.0", "Accept": "image/*,*/*;q=0.8"},
    )
    chunks: list[bytes] = []
    total = 0
    with opener.open(request_obj, timeout=TIMEOUT_S) as response:
        content_type = (response.headers.get_content_type() or "").lower()
        while True:
            chunk = response.read(64 * 1024)
            if not chunk:
                break
            total += len(chunk)
            if total > max_bytes:
                raise ValueError(f"Image is larger than the {max_bytes // (1024 * 1024)} MB limit.")
            chunks.append(chunk)
    blob = b"".join(chunks)
    if not blob:
        raise ValueError("The URL returned an empty response.")
    suffix = _sniff_suffix(blob) or MIME_TO_SUFFIX.get(content_type)
    if suffix is None:
        raise ValueError("Not a supported image (jpg, png, webp, gif or bmp).")
    _verify_image(blob)
    return blob, suffix


def _decode_paste(data_url: str, max_bytes: int) -> tuple[bytes, str]:
    match = _DATA_URL_RE.fullmatch((data_url or "").strip())
    if match is None:
        raise ValueError("No pasted image found. Click the paste box, then press Ctrl+V.")
    mime = match.group(1).lower()
    encoded = re.sub(r"\s+", "", match.group(2))
    try:
        blob = base64.b64decode(encoded)
    except (ValueError, TypeError) as exc:
        raise ValueError("Pasted image data is invalid.") from exc
    if not blob:
        raise ValueError("Pasted image is empty.")
    if len(blob) > max_bytes:
        raise ValueError(f"Pasted image is larger than the {max_bytes // (1024 * 1024)} MB limit.")
    suffix = _sniff_suffix(blob)
    if suffix is None:
        raise ValueError("Pasted data isn't a supported image (jpg, png, webp, gif or bmp).")
    expected = MIME_TO_SUFFIX.get(mime)
    if expected is not None and expected != suffix:
        raise ValueError("Pasted image type doesn't match its data.")
    _verify_image(blob)
    return blob, suffix


def _unique_path(directory: Any, hint: str, suffix: str) -> Any:
    stem = Path(secure_filename(hint or "")).stem[:80]
    if not stem:
        stem = f"import-{time.strftime('%Y%m%d-%H%M%S')}"
    candidate = directory / f"{stem}{suffix}"
    index = 1
    while candidate.exists() and index < 1000:
        candidate = directory / f"{stem}-{index}{suffix}"
        index += 1
    if candidate.exists():
        raise ValueError("Could not find a free filename in that folder.")
    return candidate


def _target_dir(folder: str, new_folder: str) -> tuple[str, Any]:
    gallery = _gallery_module()
    if gallery is None:
        raise ValueError("The picture_gallery plugin isn't installed or is disabled.")
    if new_folder:
        name = gallery.normalize_folder_name(new_folder)
        if name is None:
            raise ValueError("Folder names use lowercase letters, digits, '-' and '_'.")
        target = gallery.image_target_dir(name)
        if target is None:
            if not gallery.create_internal_folder(name):
                raise ValueError(f"Couldn't create folder '{name}' (it may already exist).")
            target = gallery.image_target_dir(name)
        if target is None:
            raise ValueError(f"Couldn't open folder '{name}' for writing.")
        return name, target
    if not folder:
        folder = _root_value(gallery)
    target = gallery.image_target_dir(folder)
    if target is None:
        raise ValueError("That folder is external/read-only, or no longer exists.")
    return folder, target


# ----- blueprint ---------------------------------------------------------


def blueprint() -> Blueprint:
    bp = Blueprint("inky_tools", __name__, template_folder="templates")

    @bp.get("/")
    def index() -> str:
        gallery = _gallery_module()
        folders: list[dict[str, str]] = []
        gallery_url = None
        if gallery is not None:
            try:
                gallery_url = url_for("picture_gallery_admin.index")
            except Exception:  # noqa: BLE001 - gallery may be old/absent
                gallery_url = None
            label = getattr(gallery, "folder_label", lambda name: str(name))
            for name in gallery.folder_names():
                if gallery.folder_is_external(name):
                    continue
                folders.append({"value": name, "label": label(name)})
        default_folder = str(_settings().get("default_folder") or "")
        if not default_folder and folders:
            root = _root_value(gallery)
            default_folder = next(
                (f["value"] for f in folders if f["value"] != root), folders[0]["value"]
            )
        tab = (request.args.get("tab") or "import").strip()
        if tab not in ("import", "ota"):
            tab = "import"
        return render_template(
            "inky_tools/index.html",
            folders=folders,
            default_folder=default_folder,
            max_upload_mb=_max_bytes() // (1024 * 1024),
            gallery_url=gallery_url,
            gallery_ready=gallery is not None,
            tab=tab,
            topic=_ota_topic(),
        )

    @bp.post("/import/save")
    def import_save():
        try:
            folder, target = _target_dir(
                (request.form.get("folder") or "").strip(),
                (request.form.get("new_folder") or "").strip(),
            )
            max_bytes = _max_bytes()
            source = (request.form.get("source") or "url").strip()
            if source == "paste":
                blob, suffix = _decode_paste(request.form.get("paste_data") or "", max_bytes)
                hint = request.form.get("filename") or f"pasted-{time.strftime('%Y%m%d-%H%M%S')}"
            elif source == "url":
                raw_url = (request.form.get("url") or "").strip()
                if not raw_url:
                    raise ValueError("Paste an image URL first.")
                if "://" not in raw_url:
                    raw_url = "https://" + raw_url
                blob, suffix = _fetch_url(raw_url, max_bytes, _allowed_hosts())
                hint = request.form.get("filename") or unquote(
                    urlsplit(raw_url).path.rsplit("/", 1)[-1]
                )
            else:
                raise ValueError("Unknown import source.")

            destination = _unique_path(target, hint, suffix)
            destination.write_bytes(blob)
            flash(f"Saved {destination.name} to gallery folder '{folder}'.", "ok")
        except ValueError as exc:
            flash(str(exc), "warn")
        except Exception as exc:  # noqa: BLE001 - surface network errors in the UI
            flash(f"Import failed: {exc}", "error")
        return redirect(url_for("inky_tools.index", tab="import"))

    @bp.post("/ota/enable")
    def ota_enable():
        error = _publish(_ota_token().encode("utf-8"))
        if error:
            flash(error, "error")
        else:
            flash(f"OTA command published to {_ota_topic()} — now press the panel button.", "ok")
        return redirect(url_for("inky_tools.index", tab="ota"))

    @bp.post("/ota/clear")
    def ota_clear():
        error = _publish(b"")
        if error:
            flash(error, "error")
        else:
            flash(f"Retained command cleared on {_ota_topic()}.", "ok")
        return redirect(url_for("inky_tools.index", tab="ota"))

    return bp
