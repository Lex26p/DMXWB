# DMXWB

DMXWB — C++20-приложение, расширяющее контроллеры **серии Wiren Board 8 (WB8)**
физическим DMX512, WB MQTT-интеграцией и Art-Net управлением.

DMXWB не заменяет Wiren Board и использует штатную Linux/MQTT/systemd/web
инфраструктуру согласно `docs/TECHNICAL_SPEC.md`. Production artifacts собираются
на ноутбуке; Docker не используется.

## Статус

Последний завершённый engineering gate:

```text
DEV-009 — Art-Net protocol core
d4ffe7ba6d9cbcc55fd012f41a9d1121f14284c0
```

DEV-009 добавил deterministic **socket-free** Art-Net 4 core. Пользователь
подтвердил успешный test gate после внесения commit. Реальный UDP runtime и
network/hardware acceptance начинаются в DEV-010.

Текущий CMake test set содержит 12 targets, включая:

```text
dmxwb.artnet_core
```

Следующий gate:

```text
DEV-010 — Art-Net runtime, reliability and Source switching
```

## Что уже подтверждено

### Physical DMX

- physical DMX512 output через встроенный RS-485 WB8;
- standard DMX512 wire format, Start Code `0x00`;
- fixed production profile `<=300 slots / 44 Hz`;
- independent absolute 44 Hz scheduler;
- whole snapshot switching only at physical frame boundary;
- serial close/reopen/recovery;
- fast WB8 transport: manual DE + hardware BREAK + physical TEMT;
- custom kernel/WBEC patch на acceptance WB8 не требуется.

Core сохраняет:

```text
kDmxMaxChannels       = 512
kDmxPhysicalMaxSlots  = 300
kDmxOutputRefreshHz   = 44
```

### Fixture / Group / Scene

- RGBW Fixture model;
- stable monotonic IDs;
- persistence;
- multiple Group membership;
- empty Groups;
- factual Group Power;
- individual saved-state restore через Group Power;
- Scene create/overwrite/apply/rename/delete;
- Scene snapshot по stable Fixture ID;
- Scene Apply не переключает Source;
- Scene Apply строит один whole DMX snapshot;
- удалённые IDs не переиспользуются;
- retained MQTT cleanup для удалённых устройств.

DEV-008 hardware acceptance:

```text
docs/DEV008_GROUP_SCENE_HARDWARE_REPORT.txt
=== DMXWB DEV-008 GROUP + SCENE HARDWARE PASS ===
```

### MQTT

Broker на WB8:

```text
127.0.0.1:1883
```

Подтверждены:

- `libmosquitto` transport;
- короткий callback: parse/enqueue;
- FIFO Controller processing;
- retained metadata/state;
- non-retained live commands;
- retained `/on` commands игнорируются;
- reconnect/resubscribe;
- full retained republish;
- graceful retained `status=off`;
- MQTT LWT `off`;
- broker loss/recovery не останавливает continuous DMX;
- `/dmxwb/config/set` + `/dmxwb/config/result`;
- atomic config transaction через canonical persistence path.

Source уже хранится и публикуется как:

```text
mqtt
artnet
```

При `Source=artnet` MQTT logical Fixture/Group/Scene model продолжает обновляться,
но не подменяет будущий Art-Net physical output MQTT snapshot-ами.

## DEV-009 Art-Net protocol core

Реализованы:

```text
include/dmxwb/artnet_core.hpp
src/artnet_core.cpp
tests/test_artnet_core.cpp
```

### Packet set

Socket-free core обрабатывает:

```text
ArtDmx
ArtPoll
ArtSync
```

и строит:

```text
ArtPollReply
```

Protocol baseline:

```text
UDP port identity:          6454 / 0x1936
minimum protocol revision: 14
Port-Address:               0..32767
ArtDmx Length:              even 2..512
network channel state:      512
physical projection:        channels 1..300
source LOST timeout:        3 s
ArtSync timeout:            4 s
PollReply RefreshRate:      44
```

Port-Address `0` остаётся явной legacy compatibility возможностью DMXWB.

### Persistent channel state

ArtDmx хранится как persistent 512-channel state.

Short ArtDmx:

```text
1..Length       -> update
Length+1..512   -> Hold Last
```

Physical snapshot содержит только first 300 channels.

До первого valid ArtDmx physical Art-Net snapshot отсутствует. Это нужно для
будущего корректного `MQTT -> ART-NET`: переключение не должно искусственно
создавать blackout/zero frame.

### Sequence

```text
Sequence 0      -> ordering disabled
Sequence 1..255 -> stale protection
FF -> 01        -> rollover
```

Missing numbers не ждутся, FIFO/reorder queue не используется.

### Source identity / conflict

Source identity:

```text
IPv4 + ArtDmx.Physical
```

States:

```text
WAITING
ACTIVE
LOST
CONFLICT
```

DMXWB не делает HTP/LTP merge. Второй source не мутирует committed output state.

После 3 s без ArtDmx source lock освобождается, но last committed state
сохраняется: no blackout и no automatic application Source switch.

### ArtSync

Startup работает asynchronously.

После valid ArtSync:

```text
ArtDmx -> staging
next ArtSync -> atomic commit
```

После 4 s без ArtSync core возвращается в asynchronous mode. ArtSync не запускает
UART и не двигает fixed physical 44 Hz schedule.

### ArtPoll / ArtPollReply

Поддержаны normal и Targeted ArtPoll.

ArtPollReply:

- рекламирует configured output Port-Address;
- содержит one output subscription;
- `RefreshRate=44`;
- позволяет отражать active Art-Net physical-output status отдельно от subscription;
- требует explicit OEM Code.

Production OEM Code не придумывается. До регистрации продукта production bundle
не должен использовать invented placeholder.

Randomized unicast reply delay и actual send выполняются только будущим network
runtime DEV-010.

## MQTT contract

System device:

```text
/devices/dmxwb/controls/status
/devices/dmxwb/controls/source
/devices/dmxwb/controls/source/on
```

Fixture:

```text
/devices/dmxwb_fixture_<stable_id>/
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

Group:

```text
/devices/dmxwb_group_<stable_id>/
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

Scene:

```text
/devices/dmxwb_scene_<stable_id>/
```

Controls:

```text
name
apply
```

Scene lifecycle:

```text
/dmxwb/scenes/create
/dmxwb/scenes/<scene_id>/overwrite
/dmxwb/scenes/<scene_id>/delete
```

Canonical retained snapshots:

```text
/dmxwb/config
/dmxwb/state
/dmxwb/status
```

Persistence remains source of truth:

```text
disk
-> C++ canonical model
-> MQTT retained representation
```

## Persistence

Canonical paths:

```text
/etc/dmxwb/config.json
/var/lib/dmxwb/state.json
```

Config transaction uses revision validation and atomic disk commit before replacing
the in-memory model.

Runtime state save schedule:

```text
last change + 2 s
OR
first dirty + 10 s
```

Graceful shutdown forces state flush.

## Production DMX transport

Preferred WB8 fast path:

```text
250000 8N2
-> save serial_rs485
-> disable automatic kernel RS-485 direction
-> RTS/DE ON
-> TIOCSBRK, BREAK >= 120 us
-> TIOCCBRK
-> MAB >= 20 us
-> Start Code + active slots
-> wait TIOCSER_TEMT
-> RTS/DE OFF
```

Original serial RS-485 settings are restored on close/stop/error.
The DEV-003 compatibility fallback remains available where fast-path ioctls are
not supported.

## Hardware acceptance baseline

```text
Controller:       Wiren Board rev. 8.5.1 (T507)
Architecture:     arm64
OS:               Debian 11 Bullseye
WB release:       wb-2606 stable
Kernel:           6.8.0-wb160
glibc:            2.31
DMX port:         /dev/ttyRS485-1 -> ttyS2
Build compiler:   Bullseye aarch64-linux-gnu-g++ 10.2.1
MQTT broker:      127.0.0.1:1883
```

Эта конфигурация подтверждает предыдущие physical/MQTT/Group/Scene gates.
Art-Net network acceptance на WB8 ещё не выполнен — это DEV-010.

## Build/test среда

C++ build/test поддерживается только на Linux. Windows/MSVC не входит в build/test
matrix.

```text
Windows host                 -> project files, ZIP, Git, WSL launch
Local Linux / WSL on laptop -> native C++ build/tests + WB8 target build
WB8                          -> runtime/hardware/integration acceptance
Docker                       -> not used
```

Native Linux:

```sh
cd /mnt/c/Projects/DMXWB
cmake -S . -B build-linux -DBUILD_TESTING=ON -DDMXWB_WARNINGS_AS_ERRORS=ON
cmake --build build-linux -j2
ctest --test-dir build-linux --output-on-failure
```

Target build:

```sh
bash tools/wb8/build_bullseye_arm64.sh
```

## Что ещё не реализовано

До соответствующих roadmap gates намеренно отсутствуют:

- real Art-Net UDP 6454 runtime/bind/rebind;
- randomized unicast ArtPollReply transmission;
- Art-Net network recovery;
- physical MQTT/ART-NET selector integration;
- real WB8 Art-Net interoperability/hardware acceptance;
- собственный static MQTT-only Web UI;
- production systemd daemon lifecycle и offline deployment bundle;
- final 24h acceptance.

## Инженерные reference-документы

- [`docs/reference/WB8_RS485_DMX.md`](docs/reference/WB8_RS485_DMX.md)
- [`docs/reference/ARTNET4_INTEGRATION.md`](docs/reference/ARTNET4_INTEGRATION.md)

`docs/reference/` не заменяет normative `TECHNICAL_SPEC.md`.

## Источник истины

Перед каждым шагом читать:

```text
AGENTS.md
docs/PROJECT_STATE.md
docs/TECHNICAL_SPEC.md
docs/ROADMAP.md
```
