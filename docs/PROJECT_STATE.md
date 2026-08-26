# PROJECT_STATE

**Last updated:** 2026-08-26

## Repository base

```text
a23f0d6628c8f18ecbb51be0a8a5b02b770a4bd8
Implement DEV-007C MQTT configuration API
```

## Last confirmed engineering PASS

```text
DEV-007 — MQTT system and Fixture integration
```

DEV-007 подтверждён host unit/integration tests, Bullseye ARM64 GCC10 cross-build и реальным WB8 MQTT + physical DMX hardware acceptance.

Roadmap PASS condition выполнен: управление реальным Fixture через WB MQTT подтверждено физически, а MQTT broker loss/recovery не остановил continuous DMX loop.

## DEV-007 result

### MQTT transport

Реализован `libmosquitto` transport для:

```text
127.0.0.1:1883
```

Target dependency:

```text
Bullseye libmosquitto 2.0.11
libmosquitto.so.1
```

Host validation использовал установленный `libmosquitto 2.0.22`.

Lifecycle:

- asynchronous connect;
- network loop в libmosquitto;
- reconnect delay;
- resubscribe после reconnect;
- retained metadata/state;
- non-retained commands;
- retained command rejection;
- full retained republish после reconnect;
- graceful retained `status=off`;
- LWT retained `off`.

MQTT command subscription использует валидный topic filter:

```text
/devices/+/controls/+/on
```

Partial wildcard `dmxwb_fixture_+` не используется, потому что MQTT `+` должен занимать полный topic level.

### Callback / Controller boundary

`MqttClient` callback:

1. получает network message;
2. проверяет payload/topic/retain;
3. разбирает либо копирует command payload;
4. кладёт команду в thread-safe FIFO queue.

Callback не выполняет:

```text
Fixture mutation
persistence file I/O
DmxOutput publication
serial I/O
```

Однопоточный Controller/runtime context извлекает очередь последовательно и применяет команды к канонической model.

### System MQTT device

System device:

```text
/devices/dmxwb
```

Controls:

```text
status
source
```

`status`:

```text
running
error
off
```

`source`:

```text
mqtt
artnet
```

Source state retained.

### Fixture MQTT contract

Device ID строится из stable Fixture ID:

```text
/devices/dmxwb_fixture_<id>
```

Controls:

```text
name
power
red
green
blue
color
brightness
temperature
reset
```

Подтверждены:

- strict payload ranges;
- UTF-8 Name;
- RGB `R;G;B`;
- Power factual state;
- factual R/G/B/Color после Brightness + Power;
- Brightness/Temperature logical settings;
- Reset stateless pushbutton;
- Fixture metadata `hidden=true`;
- retained state/metadata;
- non-retained commands;
- retained `/on` ignored.

### Controller / DMX integration

Command flow:

```text
libmosquitto callback
-> MqttCommandQueue
-> MqttRuntimeCoordinator
-> MqttController
-> PersistenceRuntime / Fixture model
-> whole DmxSnapshot
-> DmxOutput mailbox
-> physical 44 Hz RS-485 output
```

Fixture live command:

1. меняет canonical logical Fixture state;
2. marks persistence state dirty;
3. строит one whole DMX snapshot;
4. при active Source=MQTT публикует snapshot в DmxOutput;
5. публикует factual retained MQTT states.

При Source=ARTNET MQTT Fixture model продолжает принимать изменения, но не подменяет physical Art-Net output. При возврате Source в MQTT публикуется один текущий whole MQTT snapshot.

### Reconnect and broker-loss behavior

Real WB8 acceptance подтвердил:

```text
process_survives_broker_down: PASS
dmx_continues_broker_down_user: PASS
full_republish_after_reconnect: PASS
```

Continuous DMX не зависит от MQTT network cadence.

### LWT / graceful stop

Подтверждены:

```text
graceful_off_status: PASS
mqtt_lwt_off: PASS
```

При graceful shutdown runtime публикует `status=off` до disconnect. Отдельный forced termination test подтвердил broker LWT `off`.

### Canonical MQTT snapshots

Internal/web retained topics:

```text
/dmxwb/config
/dmxwb/state
/dmxwb/status
```

MQTT не используется как база восстановления. Направление source of truth:

```text
disk
-> C++ canonical model
-> MQTT retained representation
```

Stale retained commands после restart не изменяют restored disk state.

### MQTT structural config API

Изменение полной конфигурации:

```text
/dmxwb/config/set
```

Request non-retained и содержит:

```text
request_id
expected_revision
complete canonical config
```

Формат envelope:

```json
{
  "request_id": "opaque-token",
  "expected_revision": 7,
  "config": {
    "...": "complete AppConfig"
  }
}
```

Result:

```text
/dmxwb/config/result
```

Non-retained result содержит минимум:

```text
request_id
ok
revision
error_code
message
```

Controller config API не создаёт второй persistence path. Он вызывает:

```text
PersistenceRuntime::apply_config_transaction()
```

Поэтому сохраняются DEV-006 guarantees:

- exact expected_revision check;
- schema/version validation;
- reference/address validation;
- temporary model build;
- atomic config write;
- in-memory apply только после disk commit;
- stale/invalid config не разрушает working config.

После успешного structural transaction:

- публикуется новый retained `/dmxwb/config`;
- публикуется актуальный `/dmxwb/state`;
- Fixture metadata/state синхронизируются;
- создаётся whole DMX snapshot;
- retained topics удалённых Fixture IDs очищаются.

### Persistence integration

DEV-006 persistence остаётся canonical storage:

```text
/etc/dmxwb/config.json
/var/lib/dmxwb/state.json
```

State changes используют 2 s debounce / 10 s maximum dirty interval и forced flush на graceful lifecycle.

File I/O не выполняется из DMX output thread или MQTT callback.

## DEV-007 host validation

Последний user-run clean Linux test:

```text
dmxwb.unit                 PASS
dmxwb.persistence          PASS
dmxwb.persistence_storage  PASS
dmxwb.persistence_runtime  PASS
dmxwb.mqtt_contract        PASS
dmxwb.mqtt_config          PASS
dmxwb.mqtt_controller      PASS
dmxwb.mqtt_client          PASS
dmxwb.mqtt_runtime         PASS

100% tests passed
0 tests failed out of 9
```

### WB8 target compatibility build after DEV-007C

Diagnostic artifact:

```text
Target:
artifacts/wb8-bullseye-arm64/dmxwb

Architecture: ARM aarch64
Compiler: Bullseye aarch64-linux-gnu-g++ 10.2.1
Maximum required glibc: GLIBC_2.17
Dynamic dependencies:
  libpthread.so.0
  libm.so.6
  libc.so.6

SHA256:
01b9d3e4026f639135e1dea50b64cdba7c8150e95fe2b7c6193a633b3486e4d2
```

Этот executable остаётся diagnostic CLI и не является production MQTT daemon entrypoint.

DEV-007 acceptance runtime:

```text
Target:
artifacts/wb8-bullseye-arm64/dmxwb-mqtt-acceptance

Architecture: ARM aarch64
Compiler: Bullseye aarch64-linux-gnu-g++ 10.2.1
Maximum required glibc: GLIBC_2.17
Dynamic dependencies:
  libpthread.so.0
  libmosquitto.so.1
  libm.so.6
  libc.so.6

Latest DEV-007C cross-build SHA256:
ba7363035897265295cac3d3af00dd0ebe6abd86ad5efa33f278acec1767b6b8
```

## DEV-007 hardware acceptance

Report:

```text
docs/DEV007_MQTT_HARDWARE_REPORT.txt
```

Acceptance target:

```text
Controller:       Wiren Board rev. 8.5.1 (T507)
Architecture:     aarch64
OS:               Debian 11 Bullseye
WB release:       wb-2606 stable
Kernel:           6.8.0-wb160
DMX port:         /dev/ttyRS485-1 -> ttyS2
MQTT broker:      127.0.0.1:1883
Fixture start:    1
Physical refresh: 44 Hz
```

Hardware run source base:

```text
283526b7873501e665aaef1a0905f383e1a6c518
```

Hardware run acceptance artifact SHA256:

```text
b0f741c779fed9681b636b20fddc8e295270d80606883ea78eb925e752ca7341
```

Confirmed:

```text
retained_source_command_ignored: PASS
retained_fixture_command_ignored: PASS
red_user_observation: PASS
brightness_user_observation: PASS
saved_state_while_off: PASS
power_off_user_observation: PASS
power_restore_user_observation: PASS
blue_user_observation: PASS
process_survives_broker_down: PASS
dmx_continues_broker_down_user: PASS
full_republish_after_reconnect: PASS
run_a_software_acceptance: PASS
graceful_off_status: PASS
mqtt_lwt_off: PASS
final_all_off_user_observation: PASS
final_run_software_acceptance: PASS
dev007_mqtt_hardware_result: PASS
```

Main broker-loss run diagnostics:

```text
mqtt_successful_connections: 2
mqtt_publish_failures: 0
mqtt_callback_failures: 0

runtime_commands_processed: 7
runtime_commands_rejected: 0
runtime_dmx_snapshots_published: 8
runtime_dmx_publish_failures: 0
runtime_full_republishes: 2
runtime_mqtt_publish_failures: 0
runtime_state_save_failures: 0

dmx_frames_sent: 2560
dmx_open_failures: 0
dmx_send_failures: 0
dmx_recoveries: 0
dmx_missed_deadlines: 0
dmx_active_refresh_hz: 44
dmx_serial_open_after_stop: 0
software_result: PASS
```

Final report marker:

```text
=== DMXWB DEV-007 MQTT + FIXTURE HARDWARE PASS ===
```

## Physical DMX baseline remains confirmed

MQTT gate не меняет physical timing architecture:

- `kDmxMaxChannels = 512`;
- `kDmxPhysicalMaxSlots = 300`;
- `kDmxOutputRefreshHz = 44`;
- whole snapshot only at physical frame boundary;
- serial error -> close/reopen/recover latest snapshot;
- fast WB8 transport = manual DE + hardware BREAK + physical TEMT.

Broker restart acceptance подтвердил независимость physical DMX loop от MQTT availability.

## Current engineering gate

```text
DEV-008 — Groups and Scenes
```

Цель DEV-008 по roadmap — реализовать Group/Scene logic поверх уже подтверждённых Fixture/MQTT/persistence слоёв.

До DEV-008 не переносится работа Art-Net; Art-Net protocol core начинается только в DEV-009.

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
MQTT broker:      localhost:1883
```

На текущем стенде `/dev/ttyRS485-1` постоянно отключён в WB Serial Device Driver Configuration. Hardware helpers считают порт освобождённым, не спрашивают `s/p/q` и не останавливают `wb-mqtt-serial` без отдельной необходимости.

## Art-Net decisions confirmed for future gates

До DEV-009/010 остаются зафиксированы:

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
DEV-008 — Groups and Scenes
```
