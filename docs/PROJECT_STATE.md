# PROJECT_STATE

**Last updated:** 2026-08-20

## Repository base for current step

Источник истины проекта — актуальное состояние репозитория.

База текущего шага:

```text
0b8c9a3ff6f327b09181770e8f51513af122142c
Complete DEV-003 physical DMX hardware proof
```

Этот commit окончательно зафиксировал первый hardware gate проекта.

## Last confirmed engineering PASS

```text
DEV-003 — physical DMX transport proof
0b8c9a3ff6f327b09181770e8f51513af122142c
```

Подтверждены target build и физический DMX через встроенный RS-485 WB8.

Фактическая acceptance-конфигурация DEV-003:

```text
Controller:       Wiren Board rev. 8.5.1 (T507)
Architecture:     aarch64 / arm64
OS:               Debian 11 Bullseye
WB release:       wb-2606 stable
Kernel:           6.8.0-wb160
glibc:            2.31
DMX port:         /dev/ttyRS485-1 -> ttyS2
Build compiler:   Bullseye aarch64-linux-gnu-g++ 10.2.1
Docker:           not used
```

Target проекта остаётся серией WB8, а не одной моделью.

## Current engineering gate

```text
DEV-004 — continuous DMX engine, timing and serial recovery
```

Текущий implementation step внутри gate:

```text
DEV-004A — deterministic continuous-output core + host/target build proof
```

После commit SHA этого шага остаёмся в DEV-004 и переходим к:

```text
DEV-004B — WB8 continuous-output hardware smoke
```

DEV-004 получает engineering PASS только после DEV-004B.

## DEV-004 requirements used by this step

Из `TECHNICAL_SPEC.md`:

- DMX передаётся непрерывно независимо от изменения значений;
- физический DMX-цикл работает отдельным worker;
- snapshot меняется только целиком между кадрами;
- refresh range `10..44 Hz`, default `30 Hz`;
- период задаётся между началами кадров:
  - `T0`;
  - `T0 + period`;
  - `T0 + 2*period`;
- физически невозможный refresh должен отклоняться;
- serial error не завершает process;
- recovery = close -> periodic reopen -> restore serial -> current snapshot;
- stop/restart не отправляет специальный blackout frame.

## DEV-004A implementation

### DmxOutput worker

Добавлен production-style `DmxOutput`:

- владеет собственным `DmxTransportInterface`;
- запускает отдельный `std::thread`;
- повторяет текущий frame непрерывно;
- при stop только закрывает transport и не посылает zero/blackout frame;
- public publication API принимает whole `DmxSnapshot`;
- public refresh API проверяет текущую длину DMX frame.

`DmxTransport` остаётся реальной Linux-реализацией проверенного DEV-003 algorithm и теперь реализует небольшой interface для deterministic fake transport tests.

Алгоритм реального Linux transport не изменён:

```text
38400 8N2
-> 0x00 BREAK byte
-> drain
-> 250000 8N2
-> Start Code + slots
-> drain
```

### Absolute frame-start scheduler

Diagnostic `sleep_for(25ms)` DEV-003 не используется в continuous worker.

Scheduler хранит абсолютный следующий frame start:

```text
T0
T0 + period
T0 + 2 * period
...
```

Длительность отправки текущего кадра не добавляется к следующему period.

Если deadline уже пропущен, scheduler:

- увеличивает `missed_deadlines`;
- пропускает прошедший deadline;
- возвращается к той же абсолютной time grid.

### Preallocated snapshot mailbox

Timing-sensitive output loop не делает `shared_ptr` load/release на каждом frame boundary.

Добавлен preallocated triple-buffer mailbox:

```text
front  -> читает только DmxOutput
middle -> atomic published slot
back   -> пишет publisher
```

Writer может публиковать новые whole snapshots независимо от текущей отправки. Output reader переключает `front` только перед новым кадром.

Свойства:

- три заранее выделенных 512-byte channel buffers;
- no heap allocation/deallocation в frame-boundary read path;
- single output reader;
- writer serialization через короткий publish mutex вне output timing path;
- промежуточные snapshots могут быть superseded, но torn frame невозможен;
- начатый frame остаётся неизменным до завершения `send_frame()`.

Старый `DmxSnapshotPublisher` DEV-002 сохранён для compatibility/core semantics; production `DmxOutput` использует новый mailbox.

### Refresh feasibility

Добавлены:

```text
kDmxMinRefreshHz     = 10
kDmxMaxRefreshHz     = 44
kDmxDefaultRefreshHz = 30
```

`minimum_dmx_frame_time()` учитывает минимальное wire time текущего proof transport:

- BREAK byte при 38400 8N2;
- Start Code;
- `slot_count` data bytes при 250000 8N2;
- максимум фактически измеренного transport overhead.

Следствие, зафиксированное unit test:

```text
4 slots   -> 44 Hz theoretically possible
512 slots -> 44 Hz impossible even without software overhead
512 slots -> theoretical maximum 43 Hz
```

Реальный worker измеряет `send_frame()` duration и запоминает maximum observed transport overhead для последующих validation checks.

### Runtime refresh change

Запрошенный refresh читается на frame boundary.

Корректное изменение refresh:

- применяется между кадрами;
- изменяет absolute scheduling period;
- не закрывает serial;
- не вызывает transport reopen.

### Serial recovery state machine

При open failure:

```text
open failure
-> DMX error diagnostics
-> wait reopen interval
-> open again
```

При send failure:

```text
send failure
-> close serial
-> DMX error diagnostics
-> wait reopen interval
-> open again
-> continue with latest mailbox snapshot
```

Default reopen interval текущего core:

```text
250 ms
```

Это internal recovery cadence DEV-004, не user-facing setting.

### Diagnostics

`DmxOutputDiagnostics` содержит минимум:

```text
frames_sent
open_attempts
reopen_attempts
open_failures
send_failures
recoveries
missed_deadlines
refresh_rejections
active_generation
active_refresh_hz
serial_open
max_send_duration
max_transport_overhead
last_error
```

## DEV-004 continuous diagnostic CLI

Добавлен hardware-facing diagnostic, использующий новый continuous worker:

```text
--dmx-continuous-test PATTERN
--port PATH
--start-channel N
--refresh HZ
--seconds N
```

Example:

```sh
./dmxwb --dmx-continuous-test red \
    --port /dev/ttyRS485-1 \
    --start-channel 1 \
    --refresh 30 \
    --seconds 30
```

Он предназначен для DEV-004B hardware smoke и не является отдельным application runtime/UI.

Старый DEV-003 `--dmx-test` сохранён как regression diagnostic.

## Build change

DEV-004 впервые использует `std::thread`, поэтому CMake теперь явно требует:

```text
find_package(Threads REQUIRED)
Threads::Threads
```

Это важно для Bullseye/GCC10, где pthread остаётся отдельной системной glibc library.

GNU C++ runtime по-прежнему статически линкуется в WB8 target artifact; системные glibc libraries остаются target runtime dependencies.

`tools/wb8/build_bullseye_arm64.sh` переименовал только локальный host build directory в generic:

```text
build-linux-wb8
```

и больше не печатает DEV-003A-specific completion message.

`tools/wb8/verify_on_target.sh` теперь backward-compatible, но позволяет задать отдельные report name/label через:

```text
DMXWB_TARGET_REPORT
DMXWB_TARGET_REPORT_LABEL
DMXWB_TARGET_REMOTE_DIR
```

Это позволяет DEV-004A smoke не перезаписывать исторический `DEV003A_TARGET_REPORT.txt`.

## Local assistant verification

На доступном Linux выполнено:

- CMake configure;
- GCC 14.2 build;
- `DMXWB_WARNINGS_AS_ERRORS=ON`;
- CTest PASS;
- полный `dmxwb_tests` PASS;
- static GNU runtime build PASS;
- target-like CMake shape with `CMAKE_SYSTEM_NAME=Linux` PASS;
- non-Linux/unsupported transport source compile with warnings-as-errors PASS;
- `--help` показывает DEV-004 continuous diagnostic;
- missing serial path не crash-ит process: worker делает repeated open attempts и diagnostic завершается controlled FAIL;
- ThreadSanitizer run полного unit-test executable PASS, включая concurrent triple-buffer stress test;
- Bash syntax check новых/изменённых WB8 scripts PASS.

## Deterministic tests added in DEV-004A

Проверяются:

- 10..44 Hz interface limits;
- physical refresh feasibility by frame length;
- 512 slots / 44 Hz rejection;
- measured overhead participation in feasibility;
- preallocated mailbox whole-frame semantics;
- concurrent writer/reader stress without torn snapshot;
- absolute frame-start cadence without cumulative drift;
- snapshot publication during `send_frame()` affects only next frame;
- simulated send failure -> close -> reopen -> recovery;
- recovery continues current snapshot;
- runtime 30 -> 20 Hz change without serial reopen;
- missed deadline counter and return to absolute time grid.

## DEV-004A user verification

До commit текущего step требуется:

### Windows / Visual Studio 2026

```powershell
cmake -S . -B build-dev004 -DBUILD_TESTING=ON -DDMXWB_WARNINGS_AS_ERRORS=ON
cmake --build build-dev004 --config Debug
ctest --test-dir build-dev004 -C Debug --output-on-failure
.\build-dev004\Debug\dmxwb.exe --version
.\build-dev004\Debug\dmxwb.exe --help
.\build-dev004\Debug\dmxwb_tests.exe
```

### Local Linux + WB8 target binary

Existing Bullseye rootfs from DEV-003A is reused; no setup/download is required on WB8.

```sh
bash tools/wb8/build_bullseye_arm64.sh
```

Then target CLI smoke without touching RS-485:

```sh
DMXWB_TARGET_REPORT="$PWD/docs/DEV004A_TARGET_REPORT.txt" \
DMXWB_TARGET_REPORT_LABEL="DEV-004A" \
bash tools/wb8/verify_on_target.sh root@10.200.200.1
```

Required final report marker:

```text
=== DEV-004A target execution PASS ===
```

If any software/cross-build/target CLI check FAILs — remain DEV-004A.

## Next step after DEV-004A commit SHA

```text
DEV-004B — WB8 continuous-output hardware smoke
```

Planned hardware proof uses the already-built `--dmx-continuous-test` and verifies at minimum:

- stable continuous output on `/dev/ttyRS485-1`;
- no visible flicker at default 30 Hz;
- low/high supported refresh smoke;
- diagnostic counters;
- clean stop without injected blackout;
- serial remains reusable;
- no regression of DEV-003 physical RGBW mapping.

DEV-005 Fixture model remains blocked until full DEV-004 PASS.
