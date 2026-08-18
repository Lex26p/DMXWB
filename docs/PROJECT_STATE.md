# PROJECT_STATE

**Last updated:** 2026-08-18

## Current base

Подтверждённая Git база для добавления дорожной карты:

```text
bcf18dd1e2092eebdddebe60c17001cce300c174
```

После commit этого документа пользователь должен прислать новый полный SHA. Он станет базой `DEV-001`.

## Current phase

**Roadmap definition before implementation.**

Техническое задание согласовано. Репозиторий очищен от старой MOD1/WBEC исследовательской ветки. Подготовлена полная дорожная карта реализации `docs/ROADMAP.md`.

После commit roadmap реализация начинается с `DEV-001`.

## Decided

- Основное приложение: C++20.
- Целевая система: Wiren Board 8.5.1.
- Один физический DMX512 output.
- Встроенный RS-485, default `/dev/ttyRS485-1`.
- Userspace DMX transport по проверенному WB-подходу.
- Два независимых source: `WB MQTT` и `ART-NET`.
- Source переключается явно; автоматического переключения нет.
- Art-Net управляет DMX-каналами напрямую.
- WB MQTT управляет Fixture/Group/Scene моделью.
- RGBW Fixture занимает 4 последовательных адреса.
- Static web без Node.js runtime/build step.
- Web ↔ backend только через MQTT.
- Config/runtime state сохраняются на диске.
- Art-Net автоматически восстанавливается после временной потери сети/источника без restart DMXWB.
- Документация ведётся вместе с кодом.
- Разработка выполняется по gates из `docs/ROADMAP.md`.

## Confirmed

- `docs/TECHNICAL_SPEC.md` согласован пользователем.
- Repository cleanup commit получен:
  `bcf18dd1e2092eebdddebe60c17001cce300c174`.
- Старый репозиторий сохранён пользователем отдельно перед очисткой.
- Roadmap подготовлен как следующий documentation gate.

## Not yet implemented

Код приложения отсутствует намеренно.

Не реализованы:

- C++ project skeleton;
- unit test harness;
- DMX core;
- physical DMX transport;
- continuous DMX engine;
- Fixture/Group/Scene model;
- persistence;
- MQTT;
- Art-Net;
- static web;
- systemd deployment.

## Development sequence

Кратко:

```text
DEV-001 Foundation
DEV-002 DMX core
DEV-003 Physical DMX proof
DEV-004 Continuous DMX/recovery
DEV-005 Fixture RGBW
DEV-006 Persistence
DEV-007 MQTT
DEV-008 Groups/Scenes
DEV-009 Art-Net core
DEV-010 Art-Net reliability/source switching
DEV-011 Web
DEV-012 systemd/deployment/diagnostics
DEV-013 Full integration + 24h acceptance
```

Полные цели и PASS criteria — в `docs/ROADMAP.md`.

## Next gate after roadmap commit

**DEV-001 — C++ foundation, build and test harness.**

Цель:

1. создать минимальный CMake/C++20 project;
2. production target `dmxwb`;
3. отдельный unit-test target;
4. базовая структура `src/` и `tests/`;
5. clean configure/build/test;
6. никаких hardware side effects;
7. обновить документацию фактическими командами.

Не подключать serial/MQTT/Art-Net в DEV-001.

## Notes for continuation

Перед любой реализацией:

1. прочитать `AGENTS.md`;
2. прочитать `docs/PROJECT_STATE.md`;
3. прочитать `docs/TECHNICAL_SPEC.md`;
4. прочитать `docs/ROADMAP.md`;
5. работать только от нового полного SHA, присланного пользователем после commit roadmap.
