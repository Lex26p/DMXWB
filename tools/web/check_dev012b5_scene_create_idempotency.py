#!/usr/bin/env python3
from __future__ import annotations

import pathlib

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
WEB_ROOT = REPO_ROOT / "www" / "dmxwb"


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


app = (WEB_ROOT / "app.js").read_text(encoding="utf-8")
html = (WEB_ROOT / "index.html").read_text(encoding="utf-8")
persistence = (REPO_ROOT / "src" / "persistence_runtime.cpp").read_text(encoding="utf-8")
controller = (REPO_ROOT / "src" / "mqtt_controller.cpp").read_text(encoding="utf-8")
state_header = (REPO_ROOT / "include" / "dmxwb" / "persistence.hpp").read_text(encoding="utf-8")

for token in ['./styles.css?v=014a', './app.js?v=014a']:
    if token not in html:
        fail(f"index.html missing DEV-012B5.6 cache version: {token}")

for token in [
    "kSceneCreateIdempotencyCapacity",
    "SceneCreateIdempotencyRecord",
    "scene_create_idempotency",
]:
    if token not in state_header:
        fail(f"persistence model missing bounded Scene Create record: {token}")

for token in [
    "scene_create_record.has_value()",
    "next_state.scene_create_idempotency.push_back",
    "kSceneCreateIdempotencyCapacity",
    "find_scene_create_idempotency",
]:
    if token not in persistence:
        fail(f"persistence runtime missing atomic idempotency contract: {token}")

for token in [
    "runtime_.find_scene_create_idempotency",
    '"idempotency_conflict"',
    "replay->scene_id",
    "replay->revision",
    "created.scene_id",
]:
    if token not in controller:
        fail(f"MQTT Controller missing Scene Create replay contract: {token}")

for token in [
    "let uncertainSceneCreate = null",
    'pending.operation === "create"',
    "requestId: pending.requestId",
    "payload: pending.payload",
    "uncertainSceneCreate?.requestId === result.request_id",
    "uncertainSceneCreate?.payload ??",
    "Ответ потерян. Повторное создание использует тот же запрос.",
]:
    if token not in app:
        fail(f"Web missing same-request Scene Create retry contract: {token}")

if "Операцию можно безопасно повторить" in app:
    fail("Web still promises a safe new Scene lifecycle request after timeout")

print("dev012b5_scene_create_idempotency: PASS")
