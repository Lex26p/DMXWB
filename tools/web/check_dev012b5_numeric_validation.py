#!/usr/bin/env python3
from __future__ import annotations

import pathlib

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
WEB_ROOT = REPO_ROOT / "www" / "dmxwb"


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


app = (WEB_ROOT / "app.js").read_text(encoding="utf-8")
html = (WEB_ROOT / "index.html").read_text(encoding="utf-8")
styles = (WEB_ROOT / "styles.css").read_text(encoding="utf-8")

for token in ['./styles.css?v=014a', './app.js?v=014a']:
    if token not in html:
        fail(f"index.html missing DEV-012B5.7 cache version: {token}")

for token in ['./model.js?v=014a', './mqtt-client.js?v=014a']:
    if token not in app:
        fail(f"app.js missing DEV-012B5.7 cache version: {token}")

for field_id in [
    "setting-start-address",
    "setting-artnet-universe",
]:
    field_position = html.find(f'id="{field_id}"')
    if field_position < 0:
        fail(f"index.html missing numeric setting {field_id}")
    field_markup = html[field_position : field_position + 320]
    for token in [
        'type="text"',
        'inputmode="numeric"',
        'aria-describedby="settings-result"',
        'aria-invalid="false"',
    ]:
        if token not in field_markup:
            fail(f"{field_id} missing visible-invalid-input contract: {token}")

for token in [
    "const invalidNumericSettings = new Set()",
    "function validateNumericSettingsForm()",
    "fixtureCount: structuralSettings(model)?.fixtureCount ?? 0",
    'key: "startAddress"',
    "minimum: 1",
    "maximum: 300",
    'key: "artnetUniverse"',
    "maximum: 32767",
    "values.startAddress + values.fixtureCount * 4 - 1",
    'input.setAttribute("aria-invalid", "true")',
    'if (!invalidNumericSettings.has("startAddress"))',
    'if (!invalidNumericSettings.has("artnetUniverse"))',
    "!numericValidation.valid ||",
    "(!info.dirty && numericValidation.valid)",
]:
    if token not in app:
        fail(f"Web missing strict numeric validation contract: {token}")

if 'id="setting-fixture-count"' in html:
    fail("Fixture Count must not remain a user-editable numeric field")

publish_start = app.find("function publishConfigDraft()")
publish_end = app.find("function makeSceneRequestId", publish_start)
if publish_start < 0 or publish_end < 0:
    fail("cannot locate Config Set publication function")
publish = app[publish_start:publish_end]

for token in [
    "const validation = validateNumericSettingsForm();",
    "if (!validation.valid)",
    "info.proposal.fixtures?.count !== validation.values.fixtureCount",
    "info.proposal.fixtures?.start_address !== validation.values.startAddress",
    "info.proposal.artnet?.universe !== validation.values.artnetUniverse",
    "publishCommand(MQTT_CONFIG_SET_TOPIC",
]:
    if token not in publish:
        fail(f"Config Set lacks pre-publication validation: {token}")

if publish.find("validateNumericSettingsForm") > publish.find(
    "publishCommand(MQTT_CONFIG_SET_TOPIC"
):
    fail("numeric validation occurs after Config Set publication")

for stale_pattern in [
    'if (count === null) {\n    return;',
    'if (value === null) {\n    return;',
]:
    if stale_pattern in app:
        fail("numeric handler can still silently ignore a visible invalid value")

if '.form-grid input[aria-invalid="true"]' not in styles:
    fail("styles.css missing visible invalid-field treatment")

print("dev012b5_numeric_validation: PASS")
