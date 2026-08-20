# PROJECT_STATE

**Last updated:** 2026-08-19

## Current base

Последний полный SHA, явно присланный пользователем и являющийся базой текущего gate:

```text
6b6e5b8329bbf1d9c893205d60427974e8e59bd5
```

Этот SHA подтверждает PASS и commit `DEV-002 — DMX core types and deterministic frame model`, включая исправление MSVC warnings-as-errors.

## Current phase

**DEV-003 — physical DMX transport proof on `/dev/ttyRS485-1`.**

Цель gate — доказать на реальном Wiren Board 8.5.1 и реальном RGBW-светильнике, что минимальный C++ transport физически формирует рабочий DMX512 через встроенный RS-485.

Это первый обязательный hardware gate. До его фактического PASS не переходить к continuous DMX engine, Fixture model, MQTT или Art-Net.

## Implemented in current DEV-003 package

Минимальный `DmxTransport`:

- default port `/dev/ttyRS485-1`;
- Linux-only real serial backend;
- корректное освобождение file descriptor в destructor/`close()`;
- data mode `250000 8N2`;
- software BREAK по утверждённому WB-подходу:
  - line rate `38400`;
  - write `0x00`;
  - дождаться физического завершения передачи;
  - вернуть line rate `250000 8N2`;
- Start Code `0x00` передаётся отдельно от channel payload;
- frame payload берётся из существующего immutable `DmxSnapshot`/`DmxFrameView`;
- полная запись обрабатывает short writes и `EINTR`;
- ошибки открытия/config/write/drain возвращаются через `last_error()`;
- Windows/non-Linux build использует unsupported backend и не обращается к hardware.

Для точной установки нестандартной скорости `250000` Linux backend использует `termios2` + `BOTHER` и числовую скорость, а не зависит от наличия libc-константы `B250000`.

## DEV-003 diagnostic mode

В `dmxwb` добавлен отдельный diagnostic CLI:

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

Для одного RGBW fixture diagnostic mode формирует ровно четыре активных channel values начиная с `--start-channel`; предыдущие slots внутри физического frame остаются нулевыми.

Default:

```text
port          = /dev/ttyRS485-1
start-channel = 1
frames        = 120
```

Diagnostic mode повторяет один и тот же frame с приблизительной паузой 25 ms, только чтобы реальный fixture устойчиво показал фиксированный test pattern. Это не production scheduler и не реализация `DEV-004`.

## Host tests added in DEV-003

Дополнительно deterministic tests проверяют:

- parser diagnostic patterns;
- `red` mapping = `255,0,0,0`;
- `white` mapping = `0,0,0,255`;
- non-1 start channel;
- zero-filled slots перед RGBW fixture;
- generation сохранён в diagnostic snapshot;
- invalid start channel 0;
- RGBW range за channel 512 отклоняется.

Предыдущие DEV-002 tests остаются обязательными.

## Local assistant verification

На Linux development host выполнены:

- clean CMake configure;
- clean build;
- `DMXWB_WARNINGS_AS_ERRORS=ON`;
- GCC 14.2.0;
- CTest `1/1 PASS`;
- полный `dmxwb_tests` -> `All tests passed`;
- `dmxwb --version` -> `dmxwb 0.1.0`;
- `dmxwb --help` показывает DEV-003 diagnostic mode;
- попытка открыть заведомо отсутствующий serial path корректно завершается ошибкой без crash;
- non-Linux transport source отдельно проверен компилятором с warnings-as-errors.

Это подтверждает только software/build часть gate и **не заменяет hardware test на Wiren Board**.

## External basis rechecked for DEV-003

Перед реализацией повторно проверена актуальная страница Wiren Board «Прямое управление DMX-512 через встроенный RS-485 на Wiren Board».

Она подтверждает используемый proof method:

```text
250000 baud
2 stop bits
BREAK: 38400 baud + 0x00 + wait for transmission completion
continuous repeated frames
```

Также перед hardware test порт должен быть освобождён от штатного serial driver через настройки Serial-устройств Wiren Board.

Опубликованное Wiren Board решение является community solution и не считается доказательством стабильности нашей C++-реализации: именно поэтому реальный hardware PASS обязателен.

## Intentionally not implemented in DEV-003

- production `DmxOutput` worker;
- absolute frame-start scheduler;
- configurable 10/30/44 Hz refresh;
- refresh feasibility calculation;
- serial reopen/retry state machine;
- runtime error recovery;
- Fixture model и RGBW application algorithms;
- configuration/persistence;
- MQTT;
- Art-Net;
- systemd;
- web.

Эти задачи принадлежат следующим gates дорожной карты.

## DEV-003 PASS criteria

Gate получает PASS только если пользователь на реальном Wiren Board и RGBW fixture подтвердил:

- `/dev/ttyRS485-1` успешно открывается после освобождения порта;
- `all-off` физически выключает все четыре RGBW channel;
- `red` включает только R;
- `green` включает только G;
- `blue` включает только B;
- `white` включает только W;
- `all-on` устанавливает все R/G/B/W в 255;
- каждый pattern устойчиво виден без заметного flicker во время diagnostic burst;
- каждый diagnostic run завершается сообщением о корректном закрытии serial;
- после завершения процесса serial port снова доступен;
- patch kernel/WBEC не требуется.

Если любой pattern не соответствует физическому результату, есть flicker/нестабильность, port не открывается или transport выдаёт ошибку — остаёмся на `DEV-003` и исправляем transport.

## Completed gates

- Repository reset / specification cleanup.
- Workflow + development roadmap.
- `DEV-001 — C++ foundation, build and test harness` — confirmed SHA `6704b01ac25a44b5174178f52bdc7158d0295ef3`.
- `DEV-002 — DMX core types and deterministic frame model` — confirmed SHA `6b6e5b8329bbf1d9c893205d60427974e8e59bd5`.

## Next gate after hardware PASS SHA

**DEV-004 — continuous DMX engine, timing and serial recovery.**

Переход разрешён только после фактического hardware PASS DEV-003 и нового полного SHA от пользователя.
