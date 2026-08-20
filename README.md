# DMXWB

DMXWB — C++20-приложение, расширяющее контроллеры **серии Wiren Board 8 (WB8)** физическим DMX512 и будущим Art-Net/WB MQTT управлением.

DMXWB не заменяет Wiren Board и использует штатную Linux/MQTT/systemd/web инфраструктуру согласно `docs/TECHNICAL_SPEC.md`.

Контроллеру не требуется интернет для build, deployment или runtime. Production artifacts собираются на ноутбуке. Docker не используется.

## Статус

Последний завершённый engineering gate:

```text
DEV-004 — continuous DMX engine, timing and serial recovery
```

База, от которой выполнен DEV-004 closure:

```text
22d4befec0884366132355a390879bf89e8255b4
Add DEV-004 continuous DMX output core
```

DEV-004A target proof хранится в:

```text
docs/DEV004A_TARGET_REPORT.txt
```

DEV-004B production hardware acceptance хранится в:

```text
docs/DEV004B_FAST_TRANSPORT_REPORT.txt
```

Следующий gate:

```text
DEV-005 — Fixture RGBW model and addressing
```

## DEV-004 continuous engine

Реализованы:

- отдельный `DmxOutput` worker thread;
- continuous frame transmission;
- absolute frame-start cadence без cumulative send-time drift;
- refresh `10..44 Hz`, default `30 Hz`;
- measured physical refresh feasibility;
- безопасный startup: high refresh применяется только после измерения реального transport;
- runtime refresh change на frame boundary без serial reopen;
- preallocated triple-buffer snapshot mailbox;
- whole-snapshot switch только между кадрами;
- serial open/write error handling;
- close/reopen/retry и recovery с актуального snapshot;
- timing/recovery diagnostics.

## Production DMX transport

Предпочтительный WB8 fast path:

```text
250000 8N2 постоянно
-> сохранить serial_rs485
-> temporary kernel automatic RS-485 OFF
-> RTS/DE ON
-> TIOCSBRK, BREAK >= 120 us
-> TIOCCBRK
-> MAB >= 20 us
-> Start Code 0x00 + active slots
-> wait physical TEMT via TIOCSERGETLSR
-> RTS/DE OFF
```

При `close()`, normal stop и error/recovery исходная `serial_rs485` конфигурация восстанавливается.

Если требуемые standard Linux ioctl недоступны, transport автоматически использует compatibility fallback, доказанный DEV-003:

```text
38400 8N2 -> 0x00 -> drain
250000 8N2 -> Start Code + slots -> drain
```

Custom kernel patch на подтверждённой WB8 acceptance-конфигурации **не требуется**.

## DEV-004B hardware acceptance

Подтверждено production binary на:

```text
Controller:       Wiren Board rev. 8.5.1 (T507)
Architecture:     arm64
OS:               Debian 11 Bullseye
WB release:       wb-2606 stable
Kernel:           6.8.0-wb160
glibc:            2.31
DMX port:         /dev/ttyRS485-1 -> ttyS2
Build compiler:   Bullseye aarch64-linux-gnu-g++ 10.2.1
```

Ключевые результаты:

| Case | Frames | Missed deadlines | Max send | Physical result |
|---|---:|---:|---:|---|
| 512 slots / 30 Hz / RED / 60 s | 1800 | 0 | 24.004 ms | PASS, no flicker |
| 512 slots / 30 Hz / GREEN / 10 s | 300 | 0 | 24.870 ms | PASS, no flicker |
| 512 slots / 30 Hz / BLUE / 10 s | 300 | 0 | 23.284 ms | PASS, no flicker |
| 512 slots / 30 Hz / WHITE / 10 s | 300 | 0 | 23.188 ms | PASS, no flicker |
| 240 slots / 44 Hz / BLUE / 30 s | 1320 | 0 | 12.658 ms | PASS, no flicker |

Дополнительно `512 slots / requested 44 Hz` корректно отклонён после первого измерения:

```text
refresh_rejections: 1
active_refresh_hz: 30
missed_deadlines: 0
```

То есть физически невозможный high refresh не вызывает missed-deadline storm.

## Continuous diagnostic

Diagnostic CLI поддерживает принудительную длину кадра через `--slots`:

```sh
./dmxwb --dmx-continuous-test blue \
    --port /dev/ttyRS485-1 \
    --start-channel 1 \
    --slots 240 \
    --refresh 44 \
    --seconds 30
```

Основные diagnostics:

```text
frames_sent
open_failures
send_failures
recoveries
missed_deadlines
refresh_rejections
active_generation
active_refresh_hz
max_send_us
max_transport_overhead_us
```

## Поддерживаемая build/test среда

**C++ build/test поддерживается только на Linux. Windows/MSVC не входит в build/test matrix проекта.**

```text
Windows host                 -> project files, editor, ZIP, Git, WSL launch
Local Linux / WSL on laptop -> native C++ build/tests + WB8 target build
WB8                          -> runtime/hardware/integration acceptance
Docker                       -> not used
```

Native Linux tests:

```sh
cd /mnt/c/Projects/DMXWB
cmake -S . -B build-linux -DBUILD_TESTING=ON -DDMXWB_WARNINGS_AS_ERRORS=ON
cmake --build build-linux -j2
ctest --test-dir build-linux --output-on-failure
./build-linux/dmxwb_tests
```

WB8 target build:

```sh
bash tools/wb8/build_bullseye_arm64.sh
```

Target artifact:

```text
artifacts/wb8-bullseye-arm64/dmxwb
```

## Что ещё не реализовано

До соответствующих roadmap gates намеренно отсутствуют:

- RGBW Fixture model;
- configuration/persistence;
- MQTT runtime;
- Groups/Scenes;
- Art-Net;
- production systemd service;
- Web UI.

## Источник истины

Перед каждым шагом читать:

1. [`AGENTS.md`](AGENTS.md)
2. [`docs/PROJECT_STATE.md`](docs/PROJECT_STATE.md)
3. [`docs/TECHNICAL_SPEC.md`](docs/TECHNICAL_SPEC.md)
4. [`docs/ROADMAP.md`](docs/ROADMAP.md)
