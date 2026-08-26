# PROJECT_STATE

**Last updated:** 2026-08-26

## Repository base

```text
80be996746ae87c99563e852c63c0c03a7aa37d1
Integrate DEV-006 persistence runtime
```

## Last confirmed engineering PASS

```text
DEV-006 — configuration and persistence
```

DEV-006 подтверждён native Linux unit/integration tests, strict warnings-as-errors, дополнительными Clang/sanitizer checks при подготовке handoff и Bullseye ARM64 GCC10 compatibility build. Hardware/RS-485 acceptance для этого gate не требовался: persistence не меняет уже подтверждённый physical DMX transport.

## DEV-006 result

### Canonical persistence model

Реализованы два разных канонических документа:

```text
/etc/dmxwb/config.json
/var/lib/dmxwb/state.json
```

`config.json` содержит структурную конфигурацию:

- schema `version = 1`;
- config `revision`;
- DMX port;
- Art-Net universe;
- ordered Fixture records с stable ID/Name;
- Group records с member Fixture IDs;
- Scene records с Fixture snapshots;
- monotonic `next_fixture_id`, `next_group_id`, `next_scene_id`.

`state.json` содержит runtime logical state:

- schema `version = 1`;
- Source (`mqtt` / `artnet`);
- Fixture stable ID;
- `requested_power`;
- сохранённые `R/G/B/W`;
- Brightness;
- Temperature.

Art-Net transient DMX buffer в logical state persistence не входит.

### Parse / serialize / validation

Подтверждены:

- JSON round trip config/state;
- strict schema/type validation;
- unsupported version reject;
- duplicate/invalid stable IDs reject;
- Fixture addressing validation до physical slot 300;
- Group member должен ссылаться на существующий Fixture;
- Scene Fixture snapshot должен ссылаться на существующий Fixture;
- monotonic next-ID counters должны быть больше существующих IDs;
- revision mismatch отклоняется до disk/in-memory apply;
- invalid proposed config не заменяет рабочую конфигурацию.

JSON implementation compact и не добавляет новой runtime library dependency.

### Stable IDs and restart restore

`Fixture` / `FixtureCollection` получили контролируемый restore path:

- persisted Fixture ID восстанавливается без перенумерации;
- persisted `next_fixture_id` восстанавливается;
- удалённые ID после restart не становятся доступными для reuse;
- Name, Start Address и logical Fixture state восстанавливаются;
- Source восстанавливается;
- новый Fixture при config transaction получает safe default runtime state, если сохранённого state для его ID нет.

Integration test создаёт реальные временные `config.json/state.json`, меняет state, сохраняет его и создаёт новый `PersistenceRuntime` как restart simulation. После restart подтверждаются те же stable IDs, Source и Fixture state.

### Atomic file storage

Atomic write реализован как:

```text
serialize in memory
-> write <target>.tmp
-> fsync temporary file
-> close
-> rename temporary file over target
```

Подтверждено:

- старый корректный target остаётся рабочим до успешного rename;
- simulated replace/write failure не заменяет старый файл;
- temporary file очищается после failure;
- failed state save не очищает dirty flag;
- последующий retry может сохранить state.

### Dirty state scheduling

`StatePersistenceManager`:

```text
debounce delay       = 2 s after last change
max dirty interval   = 10 s after first unsaved change
```

При непрерывных изменениях deadline ограничивается первым dirty timestamp + 10 s.

`flush()` выполняет forced save dirty state; этот forced-save contract проверен integration test. Подключение реального signal/shutdown lifecycle к production Controller выполняется на этапе формирования полноценного daemon lifecycle, без переноса file I/O в DMX thread.

Persistence file I/O находится в отдельном persistence/runtime API и не вызывается из `DmxOutput` thread.

### Corrupt / missing file behavior

Config load failure или corrupt/invalid `config.json`:

```text
DMX Port          = /dev/ttyRS485-1
Art-Net Universe  = 0
Fixture Count     = 0
Start Address     = 1
```

Повреждённый config автоматически не перезаписывается.

State load failure или corrupt/invalid `state.json`:

- valid config остаётся рабочим;
- Source/Fixture runtime state создаются из safe defaults;
- ошибка остаётся доступна через startup status.

### Atomic config transaction

`PersistenceRuntime::apply_config_transaction()`:

1. проверяет `expected_revision`;
2. полностью валидирует proposed config;
3. строит complete replacement Fixture model/state;
4. atomically commit-ит новый `config.json`;
5. только после успешного disk commit заменяет рабочую in-memory configuration;
6. отмечает state dirty для последующей записи согласованного `state.json`.

Stale revision и disk failure не меняют рабочую in-memory configuration.

## DEV-006 validation

Последний user-run native Linux test:

```text
dmxwb.unit                 PASS
dmxwb.persistence          PASS
dmxwb.persistence_storage  PASS
dmxwb.persistence_runtime  PASS

100% tests passed
0 tests failed out of 4
```

Подготовительный handoff также проверял новый persistence code с GNU/Clang warnings-as-errors и ASan/UBSan.

### WB8 target compatibility build

```text
Target: /mnt/c/Projects/DMXWB/artifacts/wb8-bullseye-arm64/dmxwb
Architecture: ARM aarch64
Compiler: Bullseye aarch64-linux-gnu-g++ 10.2.1
Maximum required glibc: GLIBC_2.17
Dynamic dependencies: libpthread.so.0, libm.so.6, libc.so.6
SHA256: 01b9d3e4026f639135e1dea50b64cdba7c8150e95fe2b7c6193a633b3486e4d2
```

Artifact SHA не изменился между DEV-006A/B/runtime: текущий diagnostic `main.cpp` не вызывает `PersistenceRuntime`, поэтому linker не включает этот API в executable. Bullseye build при этом компилирует весь `dmxwb_core`, включая persistence source files; runtime behavior доказан отдельным C++ integration test.

## DEV-005 physical baseline remains confirmed

Persistence gate не менял physical DMX architecture. Остаются подтверждены:

- `kDmxMaxChannels = 512` — core/network data capacity;
- `kDmxPhysicalMaxSlots = 300` — physical product limit;
- `kDmxOutputRefreshHz = 44` — fixed production cadence;
- whole snapshot only at physical frame boundary;
- serial error -> close/reopen/recover latest snapshot;
- fast WB8 transport = manual DE + hardware BREAK + physical TEMT;
- Fixture -> FixtureCollection -> DmxSnapshot -> DmxOutput -> DmxTransport -> real RGBW fixture hardware chain.

Последний physical Fixture report:

```text
docs/DEV005_FIXTURE_HARDWARE_REPORT.txt
=== DMXWB DEV-005 FIXTURE RGBW HARDWARE PASS ===
```

## Current engineering gate

```text
DEV-007 — MQTT system and Fixture integration
```

Roadmap PASS target для DEV-007 — host/WB8 MQTT + DMX integration. Persistence уже является disk source of truth; retained MQTT не должен использоваться для восстановления модели.

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
```

На текущем стенде `/dev/ttyRS485-1` постоянно отключён в WB Serial Device Driver Configuration. Hardware helpers считают порт освобождённым, не спрашивают `s/p/q` и не останавливают `wb-mqtt-serial` без отдельной необходимости.

## Art-Net decisions confirmed from current official specification

Перед DEV-009 уже зафиксированы:

- one Art-Net Port-Address/output;
- ArtDmx Length even `2..512`;
- persistent `artnet_state[512]`;
- physical output uses only channels `1..300`;
- latest committed snapshot wins, без FIFO;
- `ArtPollReply.RefreshRate = 44`;
- ArtSync staging/release + 4 s async fallback;
- source identity = source IP + `Physical`;
- Sequence 0 disables ordering;
- multiple source policy = `CONFLICT`, no merge;
- 3 s LOST diagnostic/source-lock timeout with Hold Last;
- Port-Address 0 — DMXWB compatibility exception;
- production release требует registered Art-Net OEM Code и required credit.

## Next

```text
DEV-007 — MQTT system and Fixture integration
```
