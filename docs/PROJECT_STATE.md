# PROJECT_STATE

**Last updated:** 2026-08-19

## Current base

Confirmed Git base после repository reset, явно присланная пользователем:

```text
09dc6cf99146df93c782f68ed0ae14686e0f6314
```

Этот SHA является базой текущего документационного изменения. После применения пакета, проверки и commit пользователь присылает новый полный SHA; он становится единственной базой DEV-001.

## Current phase

**Pre-implementation specification finalization.**

Repository reset завершён. Пользователь уточнил продуктовые границы перед началом реализации:

- DMXWB является специализированной подсистемой Wiren Board, а не самостоятельной универсальной платформой;
- главная цель — стабильное управление физическим DMX512 и внешнее управление по Art-Net;
- эксплуатация предполагается в доверенной локальной LAN; собственные authentication/authorization, Mosquitto ACL и политика доступа к `/mqtt` не входят в scope DMXWB;
- установка конечного приложения на Wiren Board должна выполняться полностью офлайн, без доступа в интернет.

Текущий шаг изменяет только документацию. Код приложения после repository reset всё ещё отсутствует намеренно.

## Decided

- DMXWB — компонент программной среды Wiren Board, а не отдельная универсальная платформа.
- Главная функция — физический DMX512 output; Art-Net является полноценным внешним источником управления этим выходом.
- Целевая эксплуатация — доверенная локальная LAN; application-level authentication/authorization, Mosquitto ACL, anonymous access policy и security model `/mqtt` не проектируются в DMXWB.
- Финальная installation bundle должна устанавливаться на поддерживаемый Wiren Board полностью офлайн и не выполнять интернет-загрузок.
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
- Repository reset завершён; пользователь прислал базовый SHA `09dc6cf99146df93c782f68ed0ae14686e0f6314`.
- Пользователь подтвердил продуктовые границы, доверенную LAN-модель и обязательную полностью офлайн-установку.
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

DEV-001 начинается только после применения этого документационного пакета, его проверки, commit и получения нового полного SHA от пользователя.

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
4. работать только от нового полного SHA, присланного пользователем после commit текущего документационного изменения.
