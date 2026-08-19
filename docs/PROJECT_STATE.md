# PROJECT_STATE

**Last updated:** 2026-08-19

## Current base

Последний полный SHA, явно присланный пользователем и являющийся базой текущего документационного шага:

```text
e111c1b5fd6df1b3df3a9c8ff2a68d8c1a230616
```

После применения текущего пакета, проверки, commit и push пользователь присылает новый полный SHA. Этот новый SHA становится единственной базой `DEV-001`.

## Current phase

**Development process and roadmap finalization.**

Код приложения после repository reset ещё не реализован. Текущий шаг изменяет только служебную документацию разработки:

- `AGENTS.md` получает точный обязательный формат взаимодействия с пользователем;
- `docs/ROADMAP.md` становится основной пошаговой инструкцией реализации будущим моделям;
- roadmap синхронизируется с текущими продуктовыми границами, включая fully offline deployment;
- `DEV-001` согласуется между `ROADMAP.md` и `PROJECT_STATE.md` как узкий foundation/build/test-harness gate.

## Completed documentation steps

- Repository reset завершён.
- Утверждён единый `docs/TECHNICAL_SPEC.md`.
- Зафиксировано, что DMXWB — специализированная подсистема Wiren Board, а не самостоятельная универсальная платформа.
- Главная функция — стабильный physical DMX512 output; Art-Net — полноценный внешний источник управления тем же выходом.
- Эксплуатация предполагается в доверенной локальной LAN; authentication/authorization, Mosquitto ACL и отдельная security-модель `/mqtt` вне scope.
- Production installation bundle обязан устанавливаться полностью офлайн.
- Предыдущий документационный шаг подтверждён пользователем commit SHA `e111c1b5fd6df1b3df3a9c8ff2a68d8c1a230616`.

## Decided development protocol

Перед каждым изменением ассистент:

1. работает только от последнего полного SHA пользователя;
2. скачивает необходимые файлы с GitHub именно на этом SHA;
3. выполняет один текущий gate по `docs/ROADMAP.md`;
4. готовит финальные файлы и root-relative ZIP;
5. выдаёт пользователю handoff по точному порядку из `AGENTS.md`:
   - краткое описание;
   - ZIP;
   - `Expand-Archive`;
   - build/run/test commands только если нужны;
   - что проверить;
   - Git-блок;
6. при FAIL остаётся на текущем gate;
7. при новом полном SHA переходит к следующему gate.

Стандартные пользовательские пути на текущий момент:

```text
C:\Users\pereverworkki\Downloads
C:\Projects\DMXWB
```

## Roadmap order

```text
DEV-001  C++20/CMake foundation and test harness
DEV-002  DMX core types and deterministic frame model
DEV-003  Physical DMX transport proof on Wiren Board
DEV-004  Continuous DMX engine, timing and serial recovery
DEV-005  Fixture RGBW model and addressing
DEV-006  Configuration and persistence
DEV-007  MQTT system + Fixture integration
DEV-008  Groups and Scenes
DEV-009  Art-Net protocol core
DEV-010  Art-Net runtime, recovery and Source switching
DEV-011  Static MQTT-only Web UI
DEV-012  systemd, diagnostics and fully offline deployment
DEV-013  Full integration, offline install and 24h acceptance
```

Полный scope и PASS каждого gate находится в `docs/ROADMAP.md`.

## Not yet implemented

После reset код приложения отсутствует намеренно.

Ещё не реализованы:

- C++ project skeleton;
- unit test harness;
- deterministic DMX core;
- physical DMX transport;
- continuous DMX engine;
- Fixture/Group/Scene model;
- configuration/persistence;
- MQTT layer;
- Art-Net;
- static web;
- systemd/offline deployment bundle.

## Next gate

**DEV-001 — C++ foundation, build and test harness.**

DEV-001 начинается только после применения текущего документационного пакета, его проверки, commit/push и получения нового полного SHA от пользователя.

### Цель DEV-001

Создать минимальную воспроизводимую основу C++20-проекта без hardware side effects.

### Реализовать

- root `CMakeLists.txt`;
- C++20;
- production executable target `dmxwb`;
- отдельный unit-test target;
- базовую структуру `src/` и `tests/`;
- минимальный `main.cpp`;
- deterministic unit-test runner;
- необходимые build warnings/rules.

### Не включать в DEV-001

- serial/termios;
- физический DMX;
- полноценный `DmxSnapshot`/frame model — это `DEV-002`;
- Fixture color model — это `DEV-005`;
- MQTT;
- Art-Net;
- persistence;
- systemd;
- web.

### PASS DEV-001

- clean CMake configure;
- clean build;
- unit-test executable запускается и PASS;
- production executable не обращается к hardware;
- документация содержит фактические команды build/test.

## Notes for continuation

Перед любой реализацией следующая модель обязана:

1. прочитать `AGENTS.md`;
2. прочитать этот `PROJECT_STATE.md`;
3. прочитать `docs/TECHNICAL_SPEC.md`;
4. прочитать `docs/ROADMAP.md`;
5. взять только новый полный SHA, присланный пользователем после commit текущего шага;
6. выполнить `DEV-001` строго в его границах.
