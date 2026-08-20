# DMXWB

DMXWB — C++20-приложение, расширяющее контроллеры **серии Wiren Board 8 (WB8)** физическим DMX512 и будущим Art-Net/WB MQTT управлением.

DMXWB не заменяет Wiren Board и использует его штатную Linux/MQTT/systemd/web инфраструктуру согласно `docs/TECHNICAL_SPEC.md`.

Контроллеру не требуется интернет для build, deployment или runtime. Production artifacts собираются на ноутбуке. Docker не используется.

## Статус

Последний завершённый engineering gate:

```text
DEV-003 — physical DMX transport proof
0b8c9a3ff6f327b09181770e8f51513af122142c
```

На реальном WB8 подтверждены:

- ARM64/Bullseye target build с ноутбука;
- `/dev/ttyRS485-1`;
- `250000 8N2`;
- BREAK через `38400 + 0x00 + drain`;
- Start Code `0x00`;
- RGBW patterns `all-off/red/green/blue/white/all-on`;
- отсутствие заметного flicker в DEV-003 diagnostic burst;
- clean close/reopen;
- отсутствие необходимости kernel/WBEC patch.

Текущий gate:

```text
DEV-004 — continuous DMX engine, timing and serial recovery
```

Текущий implementation step:

```text
DEV-004A — deterministic continuous-output core + host/target build proof
```

DEV-004 ещё не считается PASS до continuous hardware smoke на WB8.

## DEV-004 continuous engine

В текущем package добавлены:

- отдельный `DmxOutput` worker thread;
- continuous frame transmission;
- absolute frame-start cadence без накопления send-time drift;
- refresh `10..44 Hz`, default `30 Hz`;
- physical refresh feasibility validation по `slot_count` и observed transport overhead;
- runtime refresh change на frame boundary без serial reopen;
- preallocated triple-buffer snapshot mailbox;
- whole-snapshot switch только между кадрами;
- serial open/write error handling;
- close/reopen/retry и recovery с актуального snapshot;
- timing/recovery counters;
- deterministic fake-clock/fake-transport tests.

Проверенная Linux transport последовательность DEV-003 не менялась.

### Snapshot path

```text
publisher
   |
   v
preallocated triple buffer
   |
   | frame boundary only
   v
DmxOutput worker
   |
   v
DmxTransport
   |
   v
WB8 RS-485 -> DMX512
```

Output timing path не делает heap allocation/deallocation для получения очередного snapshot.

## Continuous diagnostic

Для DEV-004 hardware acceptance добавлен CLI:

```sh
./dmxwb --dmx-continuous-test red \
    --port /dev/ttyRS485-1 \
    --start-channel 1 \
    --refresh 30 \
    --seconds 30
```

Он печатает:

```text
frames_sent
open_failures
send_failures
recoveries
missed_deadlines
active_generation
active_refresh_hz
max_send_us
max_transport_overhead_us
```

Это hardware diagnostic нового production-style output engine, а не будущий MQTT/Art-Net application runtime.

Старый DEV-003 regression diagnostic остаётся доступен:

```sh
./dmxwb --dmx-test red --port /dev/ttyRS485-1 --start-channel 1 --frames 120
```

## Среда разработки

```text
Windows / Visual Studio 2026 -> host development/tests
Local Linux on laptop        -> Linux tests + WB8 target build
WB8                          -> runtime/hardware/integration target
Docker                       -> not used
```

Минимальный CMake: `3.18`.

### Windows / Visual Studio 2026

```powershell
cmake -S . -B build-dev004 -DBUILD_TESTING=ON -DDMXWB_WARNINGS_AS_ERRORS=ON
cmake --build build-dev004 --config Debug
ctest --test-dir build-dev004 -C Debug --output-on-failure
.\build-dev004\Debug\dmxwb.exe --version
.\build-dev004\Debug\dmxwb.exe --help
.\build-dev004\Debug\dmxwb_tests.exe
```

Windows использует unsupported hardware backend, поэтому physical serial test выполняется только на WB8.

### Local Linux / WB8 target build

Подготовленный в DEV-003A Bullseye build rootfs используется повторно:

```sh
bash tools/wb8/build_bullseye_arm64.sh
```

Target artifact:

```text
artifacts/wb8-bullseye-arm64/dmxwb
```

Для current-step CLI smoke без доступа к serial:

```sh
DMXWB_TARGET_REPORT="$PWD/docs/DEV004A_TARGET_REPORT.txt" \
DMXWB_TARGET_REPORT_LABEL="DEV-004A" \
bash tools/wb8/verify_on_target.sh root@10.200.200.1
```

Контроллер ничего не скачивает из интернета: binary и probe передаются напрямую с ноутбука через SSH/SCP.

## Подтверждённый DEV-003 target

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

Это acceptance-конфигурация, а не ограничение проекта одной моделью WB8.

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

Источник истины — актуальный репозиторий. Пользователь самостоятельно выполняет commit/push и присылает новый полный SHA после завершения шага.
