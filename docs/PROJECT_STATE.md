# PROJECT_STATE

**Last updated:** 2026-08-18

## Current base

Confirmed Git base before cleanup:

```text
b98866beb1a67d006dd6ae2f27529a77ae5115a8
```

После применения repository reset и commit пользователь должен прислать новый полный SHA. Он станет базой первого этапа реализации.

## Current phase

**Repository reset before implementation.**

Техническое задание согласовано пользователем. Старые исследовательские файлы удаляются, чтобы они не влияли на следующие этапы и не создавали конкурирующие архитектурные источники истины.

После reset в репозитории должны остаться только:

```text
README.md
AGENTS.md
docs/PROJECT_STATE.md
docs/TECHNICAL_SPEC.md
```

## Decided

- Основное приложение: C++20.
- Целевая система: Wiren Board 8.5.1.
- Один физический DMX512 output.
- Встроенный RS-485, default `/dev/ttyRS485-1`.
- Userspace DMX transport по проверенному WB-подходу; без текущей разработки custom kernel/WBEC firmware.
- Два независимых source: `WB MQTT` и `ART-NET`.
- Source переключается явно; автоматического переключения нет.
- Art-Net управляет DMX-каналами напрямую.
- WB MQTT управляет Fixture/Group/Scene моделью.
- RGBW Fixture занимает 4 последовательных адреса.
- Static web без Node.js runtime/build step.
- Web ↔ backend только через MQTT.
- Конфигурация и runtime state сохраняются на диске.
- Art-Net должен автоматически восстанавливаться после временной потери сети/пульта без restart DMXWB.
- Документация обновляется вместе с реализацией.

## Confirmed

- Утверждён единый документ `docs/TECHNICAL_SPEC.md`.
- Пользователь просмотрел ТЗ и разрешил переход к упорядочиванию репозитория.
- Старый репозиторий сохранён пользователем отдельно перед очисткой.

## Not yet implemented

После reset код приложения отсутствует намеренно.

Ещё не реализованы:

- C++ project skeleton;
- unit test harness;
- DMX transport;
- Fixture/Group/Scene model;
- MQTT layer;
- persistence;
- Art-Net;
- static web;
- systemd deployment.

## Next gate

**DEV-001 — C++ project skeleton and deterministic core tests.**

Цель следующего шага:

1. создать минимальный CMake/C++20 project;
2. определить production/test targets;
3. создать базовые immutable `DmxSnapshot`/model types без hardware I/O;
4. подключить unit test infrastructure;
5. добавить первые deterministic tests для DMX address calculation и RGBW core;
6. обновить `README.md` и `PROJECT_STATE.md`;
7. не подключать serial/MQTT/Art-Net в этот gate.

PASS:

- clean configure/build;
- все unit tests PASS;
- no hardware side effects;
- документация соответствует фактическому состоянию.

## Notes for continuation

Перед любой реализацией:

1. прочитать `AGENTS.md`;
2. прочитать это состояние;
3. прочитать `docs/TECHNICAL_SPEC.md`;
4. работать только от полного SHA, присланного пользователем после repository reset.
