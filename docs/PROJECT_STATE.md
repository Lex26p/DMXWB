# PROJECT_STATE

**Last updated:** 2026-08-20

## Repository state

Источник истины проекта — актуальное состояние репозитория.

База текущего документационного шага — commit пользователя:

```text
e6b563da756023a3e87afe38fb25e59a98d7d21e
```

Commit завершил предыдущий шаг по обновлению workflow и WB8 development context.

Текущий пакет синхронизирует `TECHNICAL_SPEC.md` и `ROADMAP.md` с уже принятыми правилами. До пользовательского commit этого пакета базовым состоянием репозитория остаётся SHA выше.

## Last confirmed engineering PASS

Последний завершённый engineering gate:

```text
DEV-002 — DMX core types and deterministic frame model
```

Подтверждённый commit:

```text
6b6e5b8329bbf1d9c893205d60427974e8e59bd5
```

DEV-003 остаётся текущим engineering gate до реального hardware PASS.

## Documentation/platform decisions — synchronized

Согласованные условия теперь должны быть одинаково отражены в `AGENTS.md`, `README.md`, `TECHNICAL_SPEC.md` и `ROADMAP.md`:

- источник истины — актуальный репозиторий;
- пользователь самостоятельно commit/push-ит изменения;
- новый SHA подтверждает завершение конкретного шага;
- перед следующим шагом ассистент заново читает актуальный GitHub;
- target platform = контроллеры серии Wiren Board 8 (WB8), а не одна конкретная версия;
- конкретная модель WB8 и версия WB software фиксируются в hardware/integration acceptance;
- DMXWB — расширение возможностей Wiren Board, а не самостоятельная SCADA и не замена контроллера;
- Windows / Visual Studio 2026 — основная host development/test среда;
- локальный Linux на ноутбуке — Linux-specific и target build environment;
- production binary собирается на ноутбуке, не на WB8;
- Docker не используется для build/runtime/deployment/tests;
- WB8 является runtime/hardware/integration target.

## Development environment

Основная пользовательская среда:

```text
Windows
Visual Studio 2026
Project: C:\Projects\DMXWB
```

На ноутбуке доступна локальная Linux-среда.

Требуется практически определить non-Docker target build path из локального Linux в бинарник, совместимый с тестируемым WB8. Конкретный compiler/cross compiler, sysroot и ABI пока не считаются Confirmed, пока не проверены на реальном контроллере.

## Target platform

DMXWB разрабатывается для **серии Wiren Board 8 (WB8)**.

Проект не должен намеренно зависеть от одной тестовой модели или одной версии ПО, если это не реальное системное ограничение.

При hardware acceptance фиксируются минимум:

```text
WB8 model
WB software / OS version
DMX port
binary/build identity
toolchain/sysroot identity, когда применимо
```

## Current phase

**DEV-003 — physical DMX transport proof.**

Цель engineering gate — доказать на реальном WB8 и реальном RGBW fixture, что C++ transport физически формирует рабочий DMX512 через встроенный RS-485.

Текущий документационный шаг не является PASS DEV-003 и не меняет функциональную реализацию transport.

После синхронизации документации DEV-003 выполняется в двух последовательных подшагах одного gate:

```text
DEV-003A — laptop -> WB8 target build enablement
DEV-003B — physical DMX transport proof
```

Это разделение не создаёт нового продуктового gate; оно только отражает реальный workflow, в котором исходники компилируются на ноутбуке.

## Implemented in current DEV-003 code

В `master` уже присутствует пакет physical transport proof, ранее добавленный commit:

```text
bbf9f0d334564fa8ae006f9ffd3fa756aefe5cc7
Add DEV-003 physical DMX transport proof
```

Минимальный `DmxTransport`:

- default port `/dev/ttyRS485-1`;
- Linux-only real serial backend;
- корректное освобождение file descriptor;
- data mode `250000 8N2`;
- software BREAK:
  - line rate `38400`;
  - write `0x00`;
  - wait/drain;
  - возврат `250000 8N2`;
- Start Code `0x00` отдельно от channel payload;
- immutable `DmxSnapshot` / `DmxFrameView`;
- short writes и `EINTR` обработаны;
- ошибки возвращаются через `last_error()`;
- Windows/non-Linux build использует unsupported backend без hardware side effects.

Для `250000` Linux backend использует `termios2 + BOTHER`.

## DEV-003 diagnostic mode

```text
--dmx-test PATTERN
--port PATH
--start-channel N
--frames N
```

Patterns:

```text
all-off
red
green
blue
white
all-on
```

Default:

```text
port          = /dev/ttyRS485-1
start-channel = 1
frames        = 120
```

Diagnostic повторяет фиксированный frame с приблизительной паузой 25 ms. Это не production scheduler и не DEV-004.

## Existing host verification

Для текущего DEV-003 package ранее были зафиксированы:

- clean CMake configure/build;
- warnings-as-errors;
- GCC host verification;
- CTest PASS;
- `dmxwb_tests` PASS;
- `dmxwb --version`;
- `dmxwb --help`;
- error handling для отсутствующего serial path;
- non-Linux source compile check.

Эти проверки подтверждают software часть, но не target compatibility и не physical DMX.

## DEV-003A — next concrete step after documentation commit

После того как текущая документационная синхронизация будет внесена пользователем в GitHub и будет получен новый SHA:

1. определить модель/архитектуру фактического WB8, используемого для ближайшего hardware test;
2. определить установленную версию WB software/OS;
3. выбрать non-Docker target build method в локальном Linux на ноутбуке;
4. собрать `dmxwb` на ноутбуке;
5. перенести готовый binary на WB8;
6. подтвердить запуск `dmxwb --version` и `dmxwb --help` на WB8;
7. зафиксировать toolchain/ABI/build commands в документации.

Если target binary не запускается или требует несовместимые runtime libraries, остаёмся в DEV-003A и исправляем build compatibility.

## DEV-003B — physical hardware PASS criteria

После успешного DEV-003A на реальном WB8 и RGBW fixture подтвердить:

- `/dev/ttyRS485-1` открывается после освобождения порта;
- `all-off` выключает RGBW;
- `red` включает только R;
- `green` включает только G;
- `blue` включает только B;
- `white` включает только W;
- `all-on` устанавливает R/G/B/W = 255;
- pattern устойчив без заметного flicker;
- diagnostic run корректно закрывает serial;
- порт доступен после процесса;
- kernel/WBEC patch не требуется.

Если transport FAIL — остаёмся в DEV-003.

## Intentionally not implemented in DEV-003

- production `DmxOutput` worker;
- absolute frame-start scheduler;
- configurable 10/30/44 Hz refresh;
- refresh feasibility calculation;
- serial reopen/retry state machine;
- runtime error recovery;
- Fixture algorithms;
- configuration/persistence;
- MQTT;
- Art-Net;
- systemd production service;
- web.

## Completed engineering gates

- `DEV-001 — C++ foundation, build and test harness` — confirmed SHA `6704b01ac25a44b5174178f52bdc7158d0295ef3`.
- `DEV-002 — DMX core types and deterministic frame model` — confirmed SHA `6b6e5b8329bbf1d9c893205d60427974e8e59bd5`.

## Next engineering gate after DEV-003 hardware PASS

```text
DEV-004 — continuous DMX engine, timing and serial recovery
```

Переход к DEV-004 разрешён только после target build proof + фактического physical DMX PASS DEV-003 и соответствующего пользовательского commit SHA.
