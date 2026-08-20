# PROJECT_STATE

**Last updated:** 2026-08-20

## Repository base

```text
b6038b87257d50428de4875308ee3025fcf9ab57
Complete DEV-004 continuous DMX output
```

## Last confirmed engineering PASS

```text
DEV-004 — continuous DMX engine, timing and serial recovery
```

DEV-004 подтверждён host tests, Bullseye ARM64 target build и production hardware acceptance на реальном WB8.

## Post-DEV-004 physical profile decision

После DEV-004 дополнительно исследован единый фиксированный production profile вместо runtime refresh calculation.

### 512 slots / 40 Hz

Минутный production run:

```text
frames_sent: 2399
missed_deadlines: 1
active_refresh_hz: 40
max_send_us: 23932
user_observation: PASS
```

Вывод: `512/40` визуально работал, но строгий timing criterion не прошёл. 40 Hz нельзя считать гарантированным фиксированным потолком полного 512-slot кадра.

### 300 slots / 44 Hz

Выполнены два последовательных production run по 60 секунд на одном acceptance WB8.

Run 1:

```text
frames_sent: 2640
open_failures: 0
send_failures: 0
recoveries: 0
missed_deadlines: 0
refresh_rejections: 0
active_refresh_hz: 44
max_send_us: 15525
max_transport_overhead_us: 2141
visual: PASS
```

Run 2:

```text
frames_sent: 2640
open_failures: 0
send_failures: 0
recoveries: 0
missed_deadlines: 0
refresh_rejections: 0
active_refresh_hz: 44
max_send_us: 17689
max_transport_overhead_us: 4305
visual: PASS
```

Период 44 Hz ≈ `22.727 ms`; даже второй run сохранил около 5 ms запаса до deadline.

**Decided:** DMXWB сохраняет стандартный DMX512 physical layer, но production profile ограничивает физический output максимумом **300 slots** и использует фиксированные **44 Hz**. Внутренние DMX/Art-Net data structures сохраняют 512 каналов.

## Fixed-profile follow-up acceptance

После внесения fixed-profile изменений выполнена повторная проверка уже **нового production core**, а не только exploratory binary.

Build/test:

```text
Native Linux GNU 15.2.0 + warnings-as-errors: PASS
CTest:                                      PASS (1/1)
Bullseye ARM64 GCC 10.2.1 cross-build:     PASS
Maximum required glibc:                    GLIBC_2.17
```

Проверенный ARM64 artifact:

```text
SHA256: 670036f59558ad0a745c7d1f99764cb9257e08c71f42aa2f7853efc6ea83c1e0
```

WB8 production acceptance `300 slots / fixed 44 Hz / BLUE / 60 s`:

```text
frames_sent:               2640
open_failures:             0
send_failures:             0
recoveries:                0
missed_deadlines:          0
active_refresh_hz:         44
max_send_us:               16407
max_transport_overhead_us: 3023
visual:                    PASS
final all-off/reopen:      PASS
```

Final marker:

```text
=== DMXWB FIXED 300-SLOT / 44 HZ PROFILE PASS ===
```

Связанный report:

```text
docs/DMX_FIXED_PROFILE_REPORT.txt
```

**Confirmed:** fixed physical profile `<=300 slots / 44 Hz` прошёл post-change build и hardware acceptance.

## Current engineering gate

```text
DEV-005 — Fixture RGBW model and addressing
```

DEV-005 validation обязана ограничивать последний физический Fixture address значением 300.

## Build/test policy

```text
Windows host                 -> project files / ZIP / Git / WSL launch
Local Linux / WSL on laptop -> all C++ host build/tests
Bullseye cross rootfs        -> ARM64 WB8 artifact
WB8                          -> runtime/hardware/integration acceptance
Docker                       -> not used
```

Windows/MSVC не входит в поддерживаемую build/test matrix.

## Acceptance target

```text
Controller:       Wiren Board rev. 8.5.1 (T507)
Architecture:     aarch64 / arm64
OS:               Debian 11 Bullseye
WB release:       wb-2606 stable
Kernel:           6.8.0-wb160
glibc:            2.31
DMX port:         /dev/ttyRS485-1 -> ttyS2
Fixture RGBW:     start channel 1
Artifact SHA256:  670036f59558ad0a745c7d1f99764cb9257e08c71f42aa2f7853efc6ea83c1e0
```

На текущем стенде `/dev/ttyRS485-1` постоянно отключён в WB Serial Device Driver Configuration. Для hardware helpers этого стенда используется эквивалент прежнего выбора `p`; helper не должен каждый раз спрашивать `s/p/q`.

## Physical output core after this follow-up

- `kDmxMaxChannels = 512` остаётся ёмкостью core/network data;
- `kDmxPhysicalMaxSlots = 300` — отдельный physical product limit;
- `kDmxOutputRefreshHz = 44` — единственная production cadence;
- пользовательской/configurable Refresh Rate нет;
- physical mailbox отвергает snapshot >300;
- absolute 44 Hz frame-start grid сохраняется;
- no FIFO; frame boundary читает только latest whole snapshot;
- serial error -> close -> periodic reopen -> current snapshot;
- diagnostics сохраняют `frames_sent`, `missed_deadlines`, active generation, send duration/overhead, serial failures/recovery.

Legacy DEV-003 transport остаётся низкоуровневым compatibility fallback. Подтверждённый fixed profile `<=300/44` основан на fast path `manual DE + hardware BREAK + TEMT`.

## Art-Net decisions confirmed from current official specification

Перед DEV-009 перепроверена Art-Net 4 Protocol Release V1.4, Document Revision 1.4dp (23/10/2025).

Decided:

- one Art-Net Port-Address/output;
- ArtDmx Length even `2..512`;
- persistent `artnet_state[512]`;
- physical output uses only channels `1..300`;
- ArtDmx arrival never directly starts serial TX;
- no ArtDmx FIFO; latest committed snapshot wins;
- `ArtPollReply.RefreshRate = 44`;
- output universe remains advertised/subscribed even when Source=MQTT;
- ArtSync supported: async at startup, staging in sync mode, release on next ArtSync, 4 s timeout back to async;
- ArtSync accepted only from IP matching the relevant ArtDmx source;
- source identity = source IP + `Physical`;
- Sequence 0 disables ordering; non-zero sequence protects against stale/out-of-order updates without waiting for missing packets;
- multiple source policy = `CONFLICT`, no HTP/LTP merge;
- 3 s LOST diagnostic/source-lock timeout with Hold Last and no automatic source switch;
- parser accepts required minimum packet size and ignores valid trailing extension bytes rather than requiring exact UDP length;
- current Art-Net spec deprecates Port-Address 0; DMXWB keeps user value 0 only as an explicit compatibility exception;
- production distribution requires registered Art-Net OEM Code and required Art-Net credit.

## Next

```text
DEV-005 — Fixture RGBW model and addressing
```

Nearest work:

- Fixture stable ID/name/state;
- fixed RGBW 4-channel addressing;
- `start_address + count * 4 - 1 <= 300`;
- Brightness / Temperature / Power semantics;
- whole MQTT snapshot rebuild;
- Linux unit tests, then WB8 DMX smoke through fixed 44 Hz output.
