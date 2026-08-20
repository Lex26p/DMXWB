# PROJECT_STATE

**Last updated:** 2026-08-20

## Repository base for DEV-004 closure

DEV-004 closure выполнен от:

```text
22d4befec0884366132355a390879bf89e8255b4
Add DEV-004 continuous DMX output core
```

Commit, содержащий этот closure document, завершает DEV-004 и открывает DEV-005.

## Last confirmed engineering PASS

```text
DEV-004 — continuous DMX engine, timing and serial recovery
```

DEV-004 подтверждён host tests, Bullseye ARM64 target build и production hardware acceptance на реальном WB8.

Связанные reports:

```text
docs/DEV004A_TARGET_REPORT.txt
docs/DEV004B_FAST_TRANSPORT_REPORT.txt
```

## Current engineering gate

```text
DEV-005 — Fixture RGBW model and addressing
```

DEV-005 должен использовать уже доказанный `DmxOutput`/`DmxTransport`; возвращаться к изменению physical transport без нового hardware evidence не требуется.

## Build/test policy

**Decided 2026-08-20:** Windows compiler/MSVC не поддерживается и не является PASS-критерием DMXWB.

```text
Windows host                 -> project files / editor / ZIP / Git / WSL launch
Local Linux / WSL on laptop -> all C++ host build/tests
Bullseye cross rootfs        -> production-style ARM64 WB8 artifact
WB8                          -> runtime/hardware/integration acceptance
Docker                       -> not used
```

CMake намеренно принимает только Linux. Поддерживаемые host compilers: GNU C++ и Clang. Доказанный target compiler: Bullseye `aarch64-linux-gnu-g++ 10.2.1`.

## DEV-004 implementation

### Continuous output core

Confirmed:

- отдельный `DmxOutput` worker;
- continuous DMX независимо от изменения snapshot;
- absolute frame-start grid;
- refresh interface `10..44 Hz`, default `30 Hz`;
- measured refresh feasibility;
- high-refresh startup сначала использует безопасные `30 Hz`, затем применяет/отклоняет requested value по реальному transport measurement;
- runtime refresh change на frame boundary;
- preallocated triple-buffer mailbox;
- whole snapshot switch только между кадрами;
- serial open/write failure -> close -> periodic reopen -> current snapshot;
- diagnostics и missed-deadline accounting;
- stop закрывает serial без специального blackout frame.

### Production fast DMX transport

Preferred path, если serial port предоставляет стандартные Linux capabilities:

```text
250000 8N2 constantly
save original serial_rs485
kernel automatic RS-485 OFF
RTS/DE ON
TIOCSBRK
BREAK >= 120 us
TIOCCBRK
MAB >= 20 us
write Start Code + active slots
wait TIOCSERGETLSR/TIOCSER_TEMT
RTS/DE OFF
```

При close/error/recovery исходная `serial_rs485` конфигурация восстанавливается.

Если fast capabilities недоступны, остаётся DEV-003 compatibility fallback:

```text
38400 8N2 -> 0x00 -> physical drain
250000 8N2 -> Start Code + active slots -> physical drain
```

Capability определяется runtime для открытого порта; fast path не считается свойством только одной ревизии WB8.

Custom kernel patch для подтверждённой acceptance-конфигурации не требуется.

## DEV-004B production hardware acceptance

Target:

```text
Controller:       Wiren Board rev. 8.5.1 (T507)
Architecture:     aarch64 / arm64
OS:               Debian 11 Bullseye
WB release:       wb-2606 stable
Kernel:           6.8.0-wb160
glibc:            2.31
DMX port:         /dev/ttyRS485-1 -> ttyS2
Fixture RGBW:     start channel 1
Artifact SHA256:  b8d6ab3331a5324e81b8acccd4658d50a4d7536c5ad04bdb5ab989992c1c3c21
```

Production acceptance results:

| Case | Frames sent | Missed | Max send | Max measured overhead | Result |
|---|---:|---:|---:|---:|---|
| 512 slots / 30 Hz / RED / 60 s | 1800 | 0 | 24.004 ms | 1.292 ms | PASS + no flicker |
| 512 slots / 30 Hz / GREEN / 10 s | 300 | 0 | 24.870 ms | 2.158 ms | PASS + no flicker |
| 512 slots / 30 Hz / BLUE / 10 s | 300 | 0 | 23.284 ms | 0.572 ms | PASS + no flicker |
| 512 slots / 30 Hz / WHITE / 10 s | 300 | 0 | 23.188 ms | 0.476 ms | PASS + no flicker |
| 240 slots / 44 Hz / BLUE / 30 s | 1320 | 0 | 12.658 ms | 1.914 ms | PASS + no flicker |

Во всех visual cases:

```text
open_failures = 0
send_failures = 0
recoveries = 0
missed_deadlines = 0
```

### Measured rejection proof

`512 slots / requested 44 Hz / all-off`:

```text
frames_sent: 91
open_failures: 0
send_failures: 0
missed_deadlines: 0
refresh_rejections: 1
active_refresh_hz: 30
max_send_us: 23153
max_transport_overhead_us: 441
```

Это подтверждает, что high refresh сначала измеряется и физически невозможное значение отклоняется без предварительного missed-deadline storm.

### Reopen / final state

Final 512-slot `all-off` one-shot:

```text
final_all_off_reopen_check: PASS
serial_reopen_across_separate_runs: PASS
wb_mqtt_serial_restore: PASS
kernel_patch_required: NO
legacy_transport_fallback_retained: YES
```

Final marker:

```text
=== DEV-004B PRODUCTION FAST TRANSPORT PASS ===
```

## DEV-004 conclusion

**Confirmed:** DEV-004 engineering gate PASS.

Доказано:

- stable continuous DMX;
- full 512-slot default 30 Hz output;
- 240-slot 44 Hz output (60 RGBW load) на acceptance WB8;
- правильные RGBW каналы и отсутствие видимого flicker;
- clean serial close/reopen;
- measured high-refresh rejection;
- transport recovery state machine unit coverage;
- no custom WB kernel patch required.

## Next

```text
DEV-005 — Fixture RGBW model and addressing
```

Ближайшая работа:

- Fixture stable ID/name/state;
- 4-channel RGBW addressing;
- Fixture Count / Start Address validation <=512;
- Brightness / Temperature / Power semantics;
- whole mqtt snapshot rebuild;
- host tests, затем WB8 DMX smoke через уже доказанный `DmxOutput`.
