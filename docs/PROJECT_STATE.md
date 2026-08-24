# PROJECT_STATE

**Last updated:** 2026-08-24

## Repository base

```text
8a6d6212179a85b880a5eed291afee30bffa6ba0
Add Fixture model hardware acceptance DEV-005
```

## Last confirmed engineering PASS

```text
DEV-005 — Fixture RGBW model and addressing
```

DEV-005 подтверждён Linux unit tests, Bullseye ARM64 target build и физическим hardware smoke на реальном WB8 через production `DmxOutput`.

## DEV-005 result

### Fixture model

Реализованы и проверены:

- stable monotonic Fixture ID;
- изменяемый `Name`;
- `requested_power` и factual Power;
- сохранённые `R/G/B/W`;
- `Brightness 0..100`;
- `Temperature 0..100`;
- actual RGBW после Power/Brightness;
- individual RGB и Color takeover с `W = 0`;
- Temperature -> `RGB = 255`, `W = round(percent * 255 / 100)`;
- Power OFF без разрушения сохранённого состояния;
- Power ON restore;
- Reset -> ON, Brightness 100, Temperature 100, RGBW 255/255/255/255.

### Fixture collection and addressing

Подтверждены:

- `Fixture Count = 0` разрешён;
- каждый Fixture занимает ровно 4 последовательных канала RGBW;
- `fixture_start = start_address + index * 4`;
- последний физический адрес не превышает `300`;
- Start Address не меняет Fixture ID/Name/logical state;
- при уменьшении Count удаляются последние Fixture;
- удалённые ID не переиспользуются;
- whole immutable `DmxSnapshot` строится полностью из actual Fixture state.

Граничные проверки:

```text
Start 1,   Count 75 -> slots 1..300   PASS
Start 2,   Count 75 -> last slot 301  REJECT
Start 297, Count 1  -> slots 297..300 PASS
Start 298, Count 1  -> last slot 301  REJECT
```

### Host validation

DEV-005A:

```text
Native Linux build + warnings-as-errors: PASS
CTest:                                    PASS
Fixture algorithm/addressing tests:       PASS
```

Unit tests покрывают initial state, RGB takeover, Color, Temperature 0/50/100, Brightness, Power restore, factual Power при Brightness=0, Reset, physical addressing boundaries, ID reuse protection и immutable whole-snapshot rebuild.

### WB8 target build

DEV-005B production artifact:

```text
Target: /mnt/c/Projects/DMXWB/artifacts/wb8-bullseye-arm64/dmxwb
Architecture: ARM aarch64
Compiler: Bullseye aarch64-linux-gnu-g++ 10.2.1
Maximum required glibc: GLIBC_2.17
Dynamic dependencies: libpthread.so.0, libc.so.6
SHA256: ef595ec643c419254c6a9395a1c4f47c7b456e5e697872cbe217c5ab075ca30b
```

### DEV-005B hardware acceptance

Acceptance прошёл через новую production цепочку:

```text
Fixture
  -> FixtureCollection::build_snapshot()
  -> DmxSnapshot
  -> DmxOutput
  -> DmxTransport
  -> /dev/ttyRS485-1
  -> physical RGBW fixture
```

Старый diagnostic pattern generator не использовался для проверяемых Fixture states.

Проверенные состояния:

```text
red              -> 255/0/0/0         PASS
 green           -> 0/255/0/0         PASS
 blue            -> 0/0/255/0         PASS
 temperature-0   -> 255/255/255/0     PASS
 temperature-50  -> 255/255/255/128   PASS
 temperature-100 -> 255/255/255/255   PASS
 brightness-50   -> 127/127/127/127   PASS
power-off         -> 0/0/0/0           PASS
power-on-restore -> 127/127/127/127   PASS
reset             -> 255/255/255/255   PASS
final all-off    -> 0/0/0/0           PASS
```

Для каждого шага:

```text
snapshot_check:        PASS
open_failures:         0
send_failures:         0
recoveries:            0
missed_deadlines:      0
active_refresh_hz:     44
serial_open_after_stop: 0
user observation:      PASS
```

Final marker:

```text
=== DMXWB DEV-005 FIXTURE RGBW HARDWARE PASS ===
```

Report:

```text
docs/DEV005_FIXTURE_HARDWARE_REPORT.txt
```

Hardware acceptance выполнялся при `source_head = 5dc3b31e5c62011122b8246527045e3b19cb3418` с modified worktree, содержащим DEV-005B CLI/helper. Эти принятые изменения и hardware report затем зафиксированы commit `8a6d6212179a85b880a5eed291afee30bffa6ba0`.

**Confirmed:** DEV-005 model и physical output соответствуют текущему `TECHNICAL_SPEC.md` и критериям `ROADMAP.md`.

## Current engineering gate

```text
DEV-006 — configuration and persistence
```

DEV-006 должен добавить каноническую конфигурацию и runtime state без file I/O в DMX output thread.

Ближайший scope:

- `/etc/dmxwb/config.json`;
- `/var/lib/dmxwb/state.json`;
- version/revision;
- monotonic fixture/group/scene counters;
- parse/serialize и full validation before apply;
- atomic tmp + fsync + rename;
- dirty state, 2 s debounce, max 10 s dirty interval;
- forced save on graceful shutdown;
- safe defaults и corrupt state/config behavior;
- stable IDs survive restart;
- atomic config transaction.

MQTT transport в DEV-006 не добавляется.

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
Latest DEV-005 artifact SHA256:
                  ef595ec643c419254c6a9395a1c4f47c7b456e5e697872cbe217c5ab075ca30b
```

На текущем стенде `/dev/ttyRS485-1` постоянно отключён в WB Serial Device Driver Configuration. Hardware helpers считают порт освобождённым, не спрашивают `s/p/q` и не останавливают `wb-mqtt-serial` без отдельной необходимости.

## Confirmed physical output core

- `kDmxMaxChannels = 512` — core/network data capacity;
- `kDmxPhysicalMaxSlots = 300` — physical product limit;
- `kDmxOutputRefreshHz = 44` — fixed production cadence;
- physical mailbox отвергает snapshot >300;
- absolute 44 Hz frame-start grid;
- no FIFO; frame boundary использует latest whole snapshot;
- serial error -> close -> periodic reopen -> current snapshot;
- fast WB8 transport = manual DE + hardware BREAK + physical TEMT;
- legacy DEV-003 path остаётся low-level compatibility fallback.

DEV-005 подтвердил, что application-level Fixture snapshot корректно проходит через этот production output core до реального RGBW fixture.

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
- ArtSync: async startup, staging in sync mode, release on next matching ArtSync, 4 s timeout back to async;
- source identity = source IP + `Physical`;
- Sequence 0 disables ordering; non-zero Sequence protects against stale/out-of-order updates without waiting for missing packets;
- multiple source policy = `CONFLICT`, no HTP/LTP merge;
- 3 s LOST diagnostic/source-lock timeout with Hold Last and no automatic source switch;
- parser accepts required minimum packet size and ignores valid trailing extension bytes;
- Port-Address 0 remains only a DMXWB compatibility exception;
- production distribution requires registered Art-Net OEM Code and required Art-Net credit.

## Next

```text
DEV-006 — configuration and persistence
```
