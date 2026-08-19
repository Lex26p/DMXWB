# PROJECT_STATE

**Last updated:** 2026-08-19

## Current base

Последний полный SHA, явно присланный пользователем и являющийся базой текущего gate:

```text
6704b01ac25a44b5174178f52bdc7158d0295ef3
```

Этот SHA подтверждает PASS и commit `DEV-001 — C++ foundation, build and test harness`.

## Current phase

**DEV-002 — DMX core types and deterministic frame model.**

Цель gate — создать hardware-independent ядро данных, которое будущий физический DMX thread сможет получать только целым immutable snapshot.

## Implemented in current DEV-002 package

- `DmxSnapshot`:
  - фиксированное хранилище максимум 512 channels;
  - `slot_count` в диапазоне `0..512`;
  - monotonic generation/revision value;
  - one-based channel API `1..512`;
  - immutable состояние после построения;
- `DmxSnapshotBuilder` как отдельный mutable construction object;
- helper `calculate_slot_count(start, count, width)` без Fixture model;
- физический payload model:
  - Start Code `0x00` отдельно;
  - active channels `1..slot_count` отдельно;
  - channel 1 соответствует payload index 0;
- `DmxSnapshotPublisher`:
  - C++20 atomic `shared_ptr<const DmxSnapshot>`;
  - reader получает один целый snapshot;
  - уже загруженный snapshot не изменяется после последующей публикации;
  - null publication отклоняется;
- `MonotonicClock` interface и `SteadyMonotonicClock` implementation без serial/scheduling loop;
- deterministic unit tests всех требований `DEV-002`;
- CLI/README обновлены текущим gate.

## Intentionally not implemented in DEV-002

- `termios` и открытие `/dev/ttyRS485-*`;
- BREAK generation;
- физическая передача Start Code/channels;
- continuous DMX worker и refresh scheduling;
- Fixture/RGBW color algorithms;
- configuration/persistence;
- MQTT;
- Art-Net;
- systemd;
- web.

Эти функции принадлежат следующим gates дорожной карты и не должны переноситься в `DEV-002`.

## DEV-002 verification commands

Windows / Visual Studio:

```powershell
Set-Location C:\Projects\DMXWB

Remove-Item -Recurse -Force .\build -ErrorAction SilentlyContinue
cmake -S . -B build -DBUILD_TESTING=ON -DDMXWB_WARNINGS_AS_ERRORS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
.\build\Debug\dmxwb.exe --version
.\build\Debug\dmxwb_tests.exe
```

Expected CTest result:

```text
100% tests passed, 0 tests failed
```

Expected version output:

```text
dmxwb 0.1.0
```

`dmxwb_tests.exe` должен завершиться строкой:

```text
All tests passed
```

## DEV-002 deterministic test coverage

Проверяются как минимум:

- channel 1;
- channel 512;
- rejection channel 0 / 513;
- rejection `slot_count > 512`;
- `slot_count = 40` для start 1 / 10 items / width 4;
- `slot_count = 60` для start 21 / 10 items / width 4;
- выход за channel 512 отклоняется;
- `Fixture Count = 0`-эквивалент helper даёт `slot_count = 0`;
- Start Code остаётся отдельным `0x00`;
- payload index 0 соответствует DMX channel 1;
- generation и data публикуются целиком;
- удерживаемый reader-ом старый snapshot остаётся неизменным после публикации нового;
- deterministic fake monotonic clock может использовать будущий scheduler без real time.

## Local assistant verification

Подготовленный source tree проверен ассистентом на Linux host:

- clean CMake configure;
- clean build;
- `DMXWB_WARNINGS_AS_ERRORS=ON`;
- GCC 14.2.0;
- CTest `1/1 PASS`;
- `dmxwb --version` -> `dmxwb 0.1.0`;
- полный `dmxwb_tests` -> `All tests passed`.

Это не заменяет пользовательский PASS на локальном Windows/dev host.

## PASS criteria for DEV-002

Gate получает PASS только если пользователь подтвердил:

- clean configure/build завершены успешно;
- warnings-as-errors build не дал ошибок;
- CTest показывает `100% tests passed, 0 tests failed`;
- `dmxwb_tests.exe` показывает PASS всех deterministic core tests;
- executable не обращается к hardware;
- для проверки не требуются Wiren Board, serial, MQTT или Art-Net.

При FAIL остаёмся на `DEV-002` и исправляем только причину ошибки.

## Completed gates

- Repository reset / specification cleanup.
- Workflow + development roadmap.
- `DEV-001 — C++ foundation, build and test harness` — confirmed SHA `6704b01ac25a44b5174178f52bdc7158d0295ef3`.

## Next gate after PASS SHA

**DEV-003 — physical DMX transport proof on `/dev/ttyRS485-1`.**

После нового полного SHA от пользователя выполнить только scope `DEV-003` из `docs/ROADMAP.md`:

- минимальный serial transport;
- default `/dev/ttyRS485-1`;
- `250000 8N2`;
- проверенный WB BREAK method;
- Start Code + небольшой фиксированный channel payload;
- hardware test на реальном Wiren Board и RGBW fixture.

Если физический DMX не проходит hardware gate, не переходить к continuous engine, Fixture, MQTT или Art-Net.
