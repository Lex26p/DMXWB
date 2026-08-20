# PROJECT_STATE

**Last updated:** 2026-08-20

## Repository base for DEV-003 closure

Источник истины проекта — актуальное состояние репозитория.

База физического DEV-003B test:

```text
bc359169e95a118f3f999854a9cd9511258dd76c
Enable verified WB8 ARM64 cross build
```

Этот commit уже содержит подтверждённый DEV-003A target-build path и:

```text
docs/DEV003A_TARGET_REPORT.txt
```

После него в рабочем дереве выполнен DEV-003B hardware test и создан:

```text
docs/DEV003B_HARDWARE_REPORT.txt
```

Текущий closure package должен быть закоммичен вместе с этим отчётом и `tools/wb8/run_dev003b_physical_test.sh`.

## Engineering result

```text
DEV-003 — physical DMX transport proof — PASS
```

DEV-003 является первым завершённым hardware engineering gate проекта.

До closure commit последний PASS, уже отражённый в предыдущем репозиторном состоянии, был:

```text
DEV-002 — DMX core types and deterministic frame model
6b6e5b8329bbf1d9c893205d60427974e8e59bd5
```

После пользовательского commit/push текущего closure package и получения нового SHA именно DEV-003 становится последним репозиторно подтверждённым engineering PASS.

## DEV-003A — target build PASS

Фактический target:

```text
Controller: Wiren Board rev. 8.5.1 (T507)
uname:      aarch64
dpkg arch:  arm64
OS:         Debian GNU/Linux 11 (bullseye)
WB release: wb-2606 stable
Kernel:     6.8.0-wb160
glibc:      2.31
Port 1:     /dev/ttyRS485-1 -> ttyS2
Port 2:     /dev/ttyRS485-2 -> ttyS1
```

Подтверждённый build path:

```text
local Linux laptop amd64
    -> native Debian 11 Bullseye amd64 rootfs
    -> Bullseye crossbuild-essential-arm64
    -> aarch64-linux-gnu-g++ 10.2.1
    -> target ARM64 ELF
```

Target artifact для DEV-003:

```text
artifacts/wb8-bullseye-arm64/dmxwb
```

Проверено:

- native Linux CTest PASS;
- Bullseye ARM64 cross build PASS;
- ELF = AArch64;
- GNU C++ runtime статически linked;
- динамически требуется системная `libc.so.6`;
- maximum required glibc symbol = `GLIBC_2.17`;
- binary запускается на реальном WB8;
- `dmxwb --version` -> `dmxwb 0.1.0`;
- `dmxwb --help` PASS;
- Docker не использовался;
- WB8 не требовал интернет-доступа.

Фактический отчёт:

```text
docs/DEV003A_TARGET_REPORT.txt
```

## DEV-003B — physical hardware PASS

Тест выполнен:

```text
target:        root@10.200.200.1
port:          /dev/ttyRS485-1
start channel: 1
frames:        120 per pattern
```

`wb-mqtt-serial` оставался active, при этом пользователь подтвердил, что `/dev/ttyRS485-1` отключён в его Serial Device Driver Configuration и свободен для прямого доступа.

Физические результаты:

```text
all-off  -> PASS
red      -> PASS
green    -> PASS
blue     -> PASS
white    -> PASS
all-on   -> PASS
```

Для каждого pattern:

- transport завершился без ошибки;
- RGBW fixture показал ожидаемый результат;
- заметного flicker не было;
- процесс сообщил `serial port closed cleanly`.

Дополнительно:

```text
final_all_off_reopen_check:          PASS
serial_reopen_across_separate_runs: PASS
wb_mqtt_serial_restore:              PASS
kernel_or_wbec_patch_required:       NO
```

Фактический отчёт:

```text
docs/DEV003B_HARDWARE_REPORT.txt
```

Regression helper:

```text
tools/wb8/run_dev003b_physical_test.sh
```

## Confirmed physical transport

Реализация DEV-003 доказана на реальном hardware:

- default `/dev/ttyRS485-1`;
- `250000 8N2`;
- BREAK proof:
  - `38400`;
  - write `0x00`;
  - wait/drain;
  - возврат `250000 8N2`;
- Start Code `0x00`;
- immutable DMX frame payload;
- short write / `EINTR` handling;
- корректный close;
- повторное открытие порта;
- fixed RGBW diagnostic patterns;
- kernel/WBEC patch не требуется.

Конкретная модель WB8 использована для acceptance, но target проекта остаётся **серия WB8**.

## Current phase after closure commit

После пользовательского commit/push текущего DEV-003 closure package:

```text
DEV-004 — continuous DMX engine, timing and serial recovery
```

DEV-004 — следующий engineering gate.

## DEV-004 objective

Превратить доказанный one-shot/diagnostic transport в production-style независимый непрерывный DMX output engine.

Требуется реализовать отдельный `DmxOutput` worker:

- единственный владелец serial fd;
- continuous BREAK + frame transmission;
- absolute frame-start scheduling;
- refresh range `10..44 Hz`;
- default refresh `30 Hz`;
- validation физически достижимого refresh для текущего `slot_count`;
- новый immutable snapshot принимается только между кадрами;
- runtime refresh change без закрытия serial, если возможно;
- serial error detection;
- controlled close/reopen/retry;
- после recovery продолжение с актуального snapshot;
- diagnostics/counters для timing и serial recovery.

## DEV-004 technical focus

Особое внимание в DEV-004:

1. **Timing.** Diagnostic `sleep_for(25ms)` из DEV-003 не переносится в production engine. Нужен absolute frame-start cadence, чтобы длительность отправки кадра не накапливала drift.
2. **Frame boundary.** Snapshot может меняться только между целыми DMX frames.
3. **Serial ownership.** Только `DmxOutput` владеет serial fd.
4. **Recovery.** Ошибка serial не должна завершать production process; worker должен закрыть fd, повторно открыть порт и возобновить отправку текущего snapshot.
5. **Snapshot publication.** Текущий `shared_ptr` publication корректен функционально, но в timing-sensitive worker нужно отдельно оценить refcount/deallocation costs. Если требуется, DEV-004 может заменить publication primitive на preallocated double/triple buffer + atomic index/generation, сохраняя immutable whole-frame semantics.
6. **No scope jump.** Fixture/MQTT/Art-Net не добавляются до PASS DEV-004.

## DEV-004 PASS direction

Точные tests должны быть реализованы вместе с gate, но минимум потребуется подтвердить:

- deterministic host timing tests с fake/controlled clock;
- snapshot switch только на frame boundary;
- refresh validation;
- отсутствие cumulative scheduling drift;
- simulated serial failure/reopen/recovery;
- clean build/tests на Windows и Linux;
- target build для WB8 через уже доказанный DEV-003A toolchain;
- hardware continuous-output smoke на WB8;
- отсутствие заметного flicker при устойчивой длительной передаче.

## Completed gates

```text
DEV-001 — C++ foundation, build and test harness — PASS
6704b01ac25a44b5174178f52bdc7158d0295ef3

DEV-002 — DMX core types and deterministic frame model — PASS
6b6e5b8329bbf1d9c893205d60427974e8e59bd5

DEV-003 — physical DMX transport proof — engineering PASS
repository closure SHA: pending current user commit
```

После нового SHA DEV-003 closure commit будет зафиксирован здесь как последний confirmed engineering PASS.
