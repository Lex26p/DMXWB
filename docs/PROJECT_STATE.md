# PROJECT_STATE

**Last updated:** 2026-08-19

## Current base

Последний полный SHA, явно присланный пользователем и являющийся базой текущего gate:

```text
f8e95681f60a337631d7afa19df10a15a01eaab6
```

Этот SHA подтверждает завершение документационного шага с `AGENTS.md` и `docs/ROADMAP.md`. Текущий пакет реализует `DEV-001`. После его локальной проверки, commit и push пользователь присылает новый полный SHA; только после этого разрешён переход к `DEV-002`.

## Current phase

**DEV-001 — C++ foundation, build and test harness.**

Цель gate — создать минимальную воспроизводимую C++20/CMake основу без hardware side effects.

## Implemented in current DEV-001 package

- root `CMakeLists.txt`;
- CMake minimum version 3.20;
- C++20;
- static library target `dmxwb_core` для общего hardware-independent кода;
- production executable target `dmxwb`;
- отдельный test executable `dmxwb_tests`;
- CTest integration;
- базовая структура `include/dmxwb/`, `src/`, `tests/`;
- общий namespace `dmxwb`;
- минимальный `app_info` API;
- минимальный CLI `--help` / `--version`;
- compiler warnings для MSVC и GCC/Clang;
- optional `DMXWB_WARNINGS_AS_ERRORS`;
- deterministic smoke unit tests без стороннего test framework;
- CMake build directories добавлены в `.gitignore`.

## Intentionally not implemented in DEV-001

- serial/termios;
- физический DMX transport;
- `DmxSnapshot` и deterministic DMX frame model — это `DEV-002`;
- continuous DMX worker — это `DEV-004`;
- Fixture/Group/Scene model;
- configuration/persistence;
- MQTT;
- Art-Net;
- systemd;
- web.

## DEV-001 verification commands

Windows / Visual Studio:

```powershell
Set-Location C:\Projects\DMXWB

Remove-Item -Recurse -Force .\build -ErrorAction SilentlyContinue
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
.\build\Debug\dmxwb.exe --version
.\build\Debug\dmxwb.exe --help
```

Expected CTest result:

```text
100% tests passed, 0 tests failed
```

Expected version output:

```text
dmxwb 0.1.0
```

`dmxwb --help` должен явно сообщать, что runtime hardware/MQTT/Art-Net подсистемы на `DEV-001` ещё не включены.

## Local assistant verification

Подготовленный `DEV-001` source tree проверяется ассистентом на доступном Linux host через clean configure/build/CTest и запуск CLI. Это подтверждает portable CMake/C++ foundation, но не заменяет пользовательский PASS на целевой dev host.

## PASS criteria for DEV-001

Gate получает PASS только если пользователь подтвердил:

- clean CMake configure завершился успешно;
- clean Debug build завершился успешно;
- `ctest` показывает `100% tests passed, 0 tests failed`;
- `dmxwb.exe --version` выводит `dmxwb 0.1.0`;
- `dmxwb.exe --help` запускается и завершается без hardware access;
- в процессе DEV-001 не требуется Wiren Board, serial, MQTT или Art-Net.

При FAIL остаёмся на `DEV-001` и исправляем только причину ошибки.

## Completed steps before DEV-001

- Repository reset завершён.
- Утверждён единый `docs/TECHNICAL_SPEC.md`.
- Зафиксированы продуктовые границы: DMXWB — подсистема Wiren Board; physical DMX является основной функцией; Art-Net — внешний источник того же DMX output.
- Зафиксирована trusted local LAN model; authentication/authorization/ACL вне scope.
- Зафиксирована полностью offline production installation.
- Зафиксирован обязательный handoff process в `AGENTS.md`.
- Создана пошаговая дорожная карта `DEV-001`…`DEV-013`.
- Документационный workflow/roadmap step подтверждён SHA `f8e95681f60a337631d7afa19df10a15a01eaab6`.

## Next gate after PASS SHA

**DEV-002 — DMX core types and deterministic frame model.**

После получения нового полного SHA от пользователя реализовать только scope `DEV-002` из `docs/ROADMAP.md`:

- immutable `DmxSnapshot`;
- channels 1..512 и `slot_count`;
- generation/revision;
- Start Code / payload model без off-by-one;
- безопасную публикацию целого snapshot;
- scheduling/clock helper interface без serial;
- deterministic host unit tests.

Не начинать physical `termios`/BREAK до `DEV-003`.
