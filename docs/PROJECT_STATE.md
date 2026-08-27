# PROJECT_STATE

**Last updated:** 2026-08-27

## Repository base

```text
d4ffe7ba6d9cbcc55fd012f41a9d1121f14284c0
Implement DEV-009 Art-Net protocol core
```

## Last confirmed engineering PASS

```text
DEV-009 — Art-Net protocol core
```

DEV-009 реализован как deterministic socket-free Art-Net 4 parser/state machine
и внесён в `master` commit `d4ffe7ba6d9cbcc55fd012f41a9d1121f14284c0`.

Пользователь подтвердил успешный запуск требуемого test gate после наложения
DEV-009 handoff. Текущий CMake test set содержит 12 targets, включая новый:

```text
dmxwb.artnet_core
```

DEV-009 по roadmap является host protocol-core gate и не требует реального UDP/WB8
hardware acceptance. Реальная сеть, socket recovery и physical Source switching
на WB8 относятся к DEV-010 и пока не считаются Confirmed.

## DEV-009 result

### Scope

Добавлены:

```text
include/dmxwb/artnet_core.hpp
src/artnet_core.cpp
tests/test_artnet_core.cpp
```

`src/artnet_core.cpp` включён в `dmxwb_core`, а CTest получил отдельный target:

```text
dmxwb.artnet_core
```

DEV-009 намеренно не добавляет real UDP socket/runtime code.

### Protocol baseline

Зафиксировано и реализовано:

```text
Art-Net UDP port:          6454 / 0x1936
minimum protocol revision: 14
supported OpCodes:
  ArtPoll
  ArtPollReply
  ArtDmx
  ArtSync
Port-Address range:        0..32767
Port-Address 0:            explicit DMXWB legacy compatibility
ArtDmx Length:             even 2..512
network state:             512 channels
physical projection:       first 300 channels
source loss timeout:       3 s
ArtSync timeout:           4 s
PollReply RefreshRate:     44
```

Parser использует minimum-length validation. Корректные trailing bytes после
обязательной части packet не являются причиной reject.

### ArtDmx state

`ArtNetCore` хранит persistent whole network state:

```text
artnet_state[512]
```

Short ArtDmx:

```text
channels 1..Length -> update
channels Length+1..512 -> Hold Last
```

Channels `301..512` остаются частью Art-Net state, но
`build_physical_snapshot()` создаёт только 300-slot physical projection.

До первого корректного ArtDmx:

```text
build_physical_snapshot() -> no snapshot
```

Это сохраняет product requirement для будущего `MQTT -> ART-NET` switching:
если после process start ещё не было валидного ArtDmx, физический output не должен
искусственно заменяться нулевым кадром.

### Sequence

Подтверждено тестами:

```text
Sequence 0      -> ordering disabled
Sequence 1..255 -> stale/out-of-order protection
FF -> 01        -> valid rollover
gaps            -> no waiting for missing packets
```

Stale packet не заменяет более новый committed state.

Sequence tracking сбрасывается при release active Art-Net source lock.

### Source identity and conflict

Source identity:

```text
IPv4 + ArtDmx.Physical
```

State machine:

```text
WAITING
ACTIVE
LOST
CONFLICT
```

Первый valid source получает lock. Packet от другого IP либо от другого
`Physical` на том же IP не смешивается с active stream:

```text
CONFLICT
no HTP/LTP merge
second source does not mutate committed output state
```

После 3 s без ArtDmx current source считается LOST:

- committed state не очищается;
- blackout не создаётся;
- application Source автоматически не переключается;
- last committed state сохраняется;
- stale source lock освобождается;
- sequence/sync tracking для старого source сбрасывается.

После release следующий допустимый source может стать ACTIVE.

### ArtSync

Startup mode:

```text
asynchronous
```

До первого valid ArtSync:

```text
ArtDmx -> committed state
```

После valid ArtSync от relevant/current ArtDmx source IP:

```text
ArtDmx -> staging only
next ArtSync -> atomic staging -> committed
```

ArtSync не запускает UART и не меняет physical 44 Hz schedule.

Если ArtSync отсутствует 4 s:

```text
synchronous -> asynchronous
```

При наличии staged state timeout commit выполняется детерминированно перед
возвратом в async mode.

ArtSync от mismatched source IP игнорируется.

### ArtPoll and ArtPollReply

ArtPoll:

- minimum packet validation;
- protocol revision >=14;
- обычный poll требует reply;
- Targeted Mode фильтрует reply по configured Port-Address.

ArtPollReply builder:

- формирует один output subscription;
- рекламирует configured Port-Address;
- `RefreshRate = 44`;
- может отделять advertised subscription от фактического
  `artnet_output_active` state;
- production OEM Code не придумывается;
- без явно заданного `oem_code` PollReply не строится.

Randomized unicast reply scheduling `0..1 s` не входит в socket-free builder и
будет выполнено network runtime в DEV-010.

### DEV-009 test coverage

Новый test target проверяет как минимум:

- Port-Address `0` compatibility и max 15-bit boundary;
- отсутствие physical snapshot до первого valid ArtDmx;
- short common header rejection;
- invalid Art-Net ID;
- unsupported OpCode ignore;
- protocol revision below 14 rejection;
- wrong Port-Address ignore;
- odd/zero/>512 ArtDmx Length rejection;
- truncated ArtDmx rejection;
- valid trailing extension bytes;
- unused high Net bit handling;
- full 512-channel state;
- 300-channel physical projection;
- short ArtDmx Hold Last;
- Sequence rollover/stale/zero/gaps;
- IPv4 + Physical source conflict;
- 3 s LOST/source-lock release;
- ArtSync source matching, staging, atomic release and 4 s fallback;
- normal and Targeted ArtPoll;
- key ArtPollReply fields;
- explicit OEM requirement.

### Not yet Confirmed by DEV-009

DEV-009 does **not** claim:

```text
real UDP 6454 bind/rebind
actual packet receive/send on WB8
randomized PollReply network scheduling
Ethernet disconnect/reconnect recovery
interface down/up recovery
source restart/IP-change recovery over real network
physical MQTT/ART-NET source switching
real Art-Net controller interoperability
WB8 Art-Net hardware acceptance
production OEM registration
```

These belong to DEV-010 and later production work.

## Confirmed baseline retained from DEV-008

### Group / Scene

DEV-008 remains Confirmed on host, Bullseye ARM64 build and real WB8 physical DMX
with two RGBW fixtures at Start Address `1` and `5`.

Confirmed behavior includes:

- stable monotonic Group IDs and Scene IDs;
- multiple Group membership and empty Groups;
- Group controls mutate member Fixtures, no separate mixing layer;
- factual Group Power = OR(member actual Power);
- Group Power OFF preserves individual saved state;
- Group Power ON restores each member independently;
- Scene create/overwrite/apply/rename/delete;
- Scene snapshots by stable Fixture ID;
- missing historical Fixture ignored;
- later-added Fixture untouched;
- Scene Apply does not change Source;
- one whole post-mutation DMX snapshot;
- retained cleanup for removed Fixture/Group/Scene devices.

Hardware report:

```text
docs/DEV008_GROUP_SCENE_HARDWARE_REPORT.txt
=== DMXWB DEV-008 GROUP + SCENE HARDWARE PASS ===
```

Physical diagnostics from that acceptance:

```text
dmx_frames_sent: 3594
dmx_open_failures: 0
dmx_send_failures: 0
dmx_recoveries: 0
dmx_missed_deadlines: 0
dmx_active_refresh_hz: 44
dmx_serial_open_after_stop: 0
```

## Confirmed MQTT baseline retained from DEV-007

Broker/runtime contract remains:

```text
127.0.0.1:1883
```

Flow:

```text
libmosquitto callback
-> MqttCommandQueue
-> MqttRuntimeCoordinator
-> MqttController
-> PersistenceRuntime / Fixture / Group / Scene
-> whole mqtt DmxSnapshot
-> DmxOutput mailbox only when Source=MQTT
```

MQTT callback does not perform:

```text
Fixture/Group/Scene mutation
persistence file I/O
serial I/O
DmxOutput operations
```

Retained MQTT is a representation, not persistence source of truth:

```text
disk
-> C++ canonical model
-> MQTT retained representation
```

Broker loss/recovery does not stop continuous physical DMX. Reconnect performs
full retained republish.

MQTT already persists and exposes:

```text
source = mqtt | artnet
```

While Source=ARTNET, MQTT logical state continues to update in background without
publishing fake MQTT physical snapshots. Returning to MQTT publishes the current
whole MQTT snapshot.

## Confirmed physical DMX baseline

```text
kDmxMaxChannels       = 512
kDmxPhysicalMaxSlots  = 300
kDmxOutputRefreshHz   = 44
```

Physical output guarantees already confirmed:

- whole snapshot only at physical frame boundary;
- fixed absolute 44 Hz schedule;
- preallocated mailbox without torn frames;
- continuous output independent of MQTT/network cadence;
- serial failure -> close/reopen/recover latest snapshot;
- preferred WB8 fast path = manual DE + hardware BREAK + TEMT;
- no custom kernel/WBEC patch required on the acceptance WB8.

Current tested hardware baseline:

```text
Controller:       Wiren Board rev. 8.5.1 (T507)
Architecture:     aarch64 / arm64
OS:               Debian 11 Bullseye
WB release:       wb-2606 stable
Kernel:           6.8.0-wb160
glibc:            2.31
DMX port:         /dev/ttyRS485-1 -> ttyS2
MQTT broker:      localhost:1883
```

`/dev/ttyRS485-1` is currently permanently disabled in WB Serial Device Driver
Configuration on the acceptance stand; existing hardware helpers therefore treat
the port as free.

## Current engineering gate

```text
DEV-010 — Art-Net runtime, reliability and Source switching
```

Goal:

- connect the confirmed DEV-009 protocol core to real IPv4 UDP 6454;
- keep Art-Net receiver active regardless of application Source;
- implement non-blocking randomized unicast ArtPollReply scheduling;
- bind/rebind and network recovery without process restart;
- publish only latest committed Art-Net physical snapshot, never FIFO;
- integrate explicit MQTT/ART-NET selector at physical frame boundaries;
- preserve current physical output if ART-NET is selected before first valid ArtDmx;
- keep Hold Last when Art-Net is LOST;
- prove temporary network failure recovery on real WB8;
- verify with a named/versioned external Art-Net source/controller.

## Build/test policy

```text
Windows host                 -> project files / ZIP / Git / WSL launch
Local Linux / WSL on laptop -> all C++ host build/tests
Bullseye cross rootfs        -> ARM64 WB8 artifact
WB8                          -> runtime/hardware/integration acceptance
Docker                       -> not used
```

Windows/MSVC is not part of the supported build/test matrix.

## Next

```text
DEV-010 — Art-Net runtime, reliability and Source switching
```
