#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
WEB_ROOT = REPO_ROOT / "www" / "dmxwb"


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


prerequisite = REPO_ROOT / "tools" / "web" / "check_dev011g1a_diagnostics.py"
result = subprocess.run([sys.executable, str(prerequisite)], cwd=REPO_ROOT)
if result.returncode != 0:
    raise SystemExit(result.returncode)

html = (WEB_ROOT / "index.html").read_text(encoding="utf-8")
app = (WEB_ROOT / "app.js").read_text(encoding="utf-8")
model = (WEB_ROOT / "model.js").read_text(encoding="utf-8")

for token in [
    "Порт DMX",
    "Светильники",
    "Добавить светильник",
    "Начальный адрес",
    "Вселенная Art-Net",
    "Приложение",
    "Конфигурация",
    "Последняя ошибка",
]:
    if token not in html:
        fail(f"index.html missing Russian UI label: {token}")

for token in [
    'powerText.textContent = "Питание"',
    "Вселенная Art-Net 0 — режим совместимости с нумерацией с нуля.",
    "Некорректный формат конфигурации.",
    "Параметры конфигурации не прошли проверку.",
    "Сцена не найдена.",
    "Не удалось выполнить операцию со сценой.",
]:
    if token not in app:
        fail(f"app.js missing Russian UI text: {token}")

for token in [
    "Светильник ${id}",
    "Группа ${id}",
    "Сцена ${scene.id}",
    'running: "Работает"',
    'connected: "Подключён"',
    'offline: "Нет связи"',
    'WAITING: "Ожидание"',
    'CONFLICT: "Конфликт"',
]:
    if token not in model:
        fail(f"model.js missing Russian fallback/status text: {token}")

# These were actual visible English strings before this localization gate.
for token in [
    ">DMX Port<",
    ">Fixture Count<",
    ">Start Address<",
    ">Art-Net Universe<",
    'powerText.textContent = "Power"',
    "legacy compatibility",
    "`Fixture ${id}`",
    "`Group ${id}`",
    "`Scene ${scene.id}`",
]:
    if token in html or token in app or token in model:
        fail(f"visible English UI residue remains: {token}")

# Backend messages may be English; the browser must map public error_code to
# Russian user-facing text instead of displaying result.message verbatim.
if "text: result.message" in app:
    fail("backend result.message is still exposed directly to the user")

for technical_name in ["DMX", "MQTT", "Art-Net", "WB MQTT", "ART-NET"]:
    if technical_name not in html:
        fail(f"protocol/product name unexpectedly removed: {technical_name}")

print("dev011ru1_navigation_and_settings_labels_russian: PASS")
print("dev011ru1_fixture_group_scene_ui_russian: PASS")
print("dev011ru1_status_values_russian: PASS")
print("dev011ru1_config_and_scene_errors_russian: PASS")
print("dev011ru1_protocol_names_preserved: PASS")
print("dev011ru1_no_known_visible_english_residue: PASS")
print("=== DMXWB DEV-011 RUSSIAN WEB UI PASS ===")
