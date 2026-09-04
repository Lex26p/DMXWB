#!/usr/bin/env python3
from __future__ import annotations

import contextlib
import functools
import http.server
import pathlib
import re
import socketserver
import threading
import urllib.request

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
WEB_ROOT = REPO_ROOT / "www" / "dmxwb"

MANDATORY = [
    "index.html",
    "app.js",
    "model.js",
    "mqtt-client.js",
    "styles.css",
]

def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")

for name in MANDATORY:
    path = WEB_ROOT / name
    if not path.is_file():
        fail(f"missing {path.relative_to(REPO_ROOT)}")

print("dev011a_mandatory_files: PASS")

texts = {
    name: (WEB_ROOT / name).read_text(encoding="utf-8")
    for name in MANDATORY
}

all_text = "\n".join(texts.values())

external_patterns = [
    r"https?://",
    r"//cdn\.",
    r"unpkg\.",
    r"jsdelivr\.",
    r"cdnjs\.",
    r"fonts\.googleapis\.",
]
for pattern in external_patterns:
    if re.search(pattern, all_text, re.IGNORECASE):
        fail(f"external dependency/reference matched {pattern}")

if (WEB_ROOT / "package.json").exists() or (WEB_ROOT / "node_modules").exists():
    fail("Node/npm artifact found in static web tree")

print("dev011a_no_external_dependencies: PASS")

backend_api_text = (
    all_text
    .replace("/dev/ttyRS485-1", "")
    .replace("/dev/ttyRS485-2", "")
)

for forbidden in [
    "/dev/tty",
    "/etc/dmxwb",
    "/var/lib/dmxwb",
    "systemctl",
    "navigator.serial",
    "XMLHttpRequest",
    "EventSource(",
    "fetch(",
]:
    if forbidden in backend_api_text:
        fail(f"direct backend/system API reference found: {forbidden}")

print("dev011a_no_direct_serial_file_systemd_api: PASS")

html = texts["index.html"]
for label in [
    "Управление",
    "Светильники и группы",
    "Сцены",
    "Настройки",
]:
    if label not in html:
        fail(f"missing required section label: {label}")

for section in ["control", "fixtures", "scenes", "settings"]:
    if f'data-section="{section}"' not in html:
        fail(f"missing section container: {section}")

if 'type="module" src="./app.js' not in html:
    fail("index.html does not load local app.js as an ES module")
if 'href="./styles.css' not in html:
    fail("index.html does not load local styles.css")

print("dev011a_required_sections: PASS")

if '?v=014a' not in html:
    fail("index.html does not cache-bust local static assets")
if './model.js?v=014a' not in texts["app.js"]:
    fail("app.js does not cache-bust model.js")
if './mqtt-client.js?v=014a' not in texts["app.js"]:
    fail("app.js does not cache-bust mqtt-client.js")

print("dev011a_asset_cache_busting: PASS")

for forbidden_ui_text in [
    "DEV-011A",
    "static foundation",
    "static/offline",
    "Без Node.js",
    "MQTT будет подключён",
    "MQTT — DEV-011B",
]:
    if forbidden_ui_text in html:
        fail(f"development-only UI text found: {forbidden_ui_text}")

print("dev011a_no_development_ui_copy: PASS")

app = texts["app.js"]
for local_import in ["./model.js", "./mqtt-client.js"]:
    if local_import not in app:
        fail(f"app.js missing local import {local_import}")

mqtt = texts["mqtt-client.js"]
for required in [
    'MQTT_WEBSOCKET_PATH = "/mqtt"',
    'locationLike.host',
    '"wss:"',
    '"ws:"',
]:
    if required not in mqtt:
        fail(f"mqtt-client.js missing endpoint contract token: {required}")

print("dev011a_local_module_contract: PASS")

class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format: str, *args: object) -> None:
        pass

handler = functools.partial(QuietHandler, directory=str(REPO_ROOT / "www"))
with socketserver.TCPServer(("127.0.0.1", 0), handler) as server:
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        for name in MANDATORY:
            url_name = "" if name == "index.html" else name
            url = f"http://127.0.0.1:{port}/dmxwb/{url_name}"
            with urllib.request.urlopen(url, timeout=3) as response:
                if response.status != 200:
                    fail(f"HTTP {response.status} for {url}")
                if not response.read():
                    fail(f"empty response for {url}")
    finally:
        server.shutdown()
        thread.join(timeout=3)

print("dev011a_static_http_load: PASS")
print("dev011a_runtime_dependencies: PASS (python checker only; web runtime is static browser files)")
print("=== DMXWB DEV-011A STATIC WEB FOUNDATION PASS ===")
