# DMXWB

DMXWB — C++20-приложение, расширяющее контроллеры **серии Wiren Board 8 (WB8)**
физическим DMX512, WB MQTT-интеграцией и Art-Net управлением.

DMXWB не заменяет Wiren Board и использует штатную Linux/MQTT/systemd/web
инфраструктуру согласно `docs/TECHNICAL_SPEC.md`. Production artifacts собираются
на ноутбуке; Docker не используется.

## Статус

Последний завершённый engineering gate:

```text
DEV-010 — Art-Net runtime, reliability and Source switching
acceptance base: fe64b3627fdad9d8905ecbb9a5540cd80a364eff
```

DEV-010 подтверждён на реальном WB8 с физическим DMX и QLC+ 5.2.2. Проверены
discovery/subscription, real ArtDmx, ArtSync, MQTT/ART-NET source switching,
Hold Last, network/interface recovery, controller restart, real source IPv4
change, repeated reconnect, conflict/no-merge и latest-state/no-FIFO behaviour.

Следующий engineering gate:

```text
DEV-011 — static MQTT-only Web UI
```

Подробная текущая точка проекта находится в
[`docs/PROJECT_STATE.md`](docs/PROJECT_STATE.md).

## Подтверждённая архитектура

```text
                         Browser
                            |
                     future DEV-011
                            |
                         MQTT
                            |
        +-------------------+-------------------+
        |                                       |
        v                                       v
Fixture / Group / Scene                 Art-Net UDP 6454
        |                                       |
        v                                       v
 MQTT whole snapshot                    ArtNetRuntime
        |                                       |
        |                                 ArtNetCore
        |                                       |
        |                             latest whole snapshot
        |                                       |
        +-------------------+-------------------+
                            |
                     DmxSourceRouter
                      mqtt | artnet
                            |
                       DmxOutput
                     fixed 44 Hz
                            |
                    /dev/ttyRS485-*
                            |
                          DMX512
```

Ключевые инварианты:

- один физический DMX output;
- только `DmxOutput` владеет serial-портом;
- MQTT/Art-Net не пишут в serial напрямую;
- physical output использует только один явно выбранный Source;
- автоматического source switching нет;
- MQTT и Art-Net продолжают обновляться в background независимо от выбранного
  Source;
- source switching происходит целыми snapshot-ами на physical frame boundary;
- ArtDmx не запускает UART;
- Art-Net использует latest committed state, не FIFO;
- LOST сохраняет Source=ART-NET и Hold Last;
- physical output фиксирован на 44 Hz;
- Art-Net network state хранит 512 channels, physical product profile использует
  только `1..300`.

## Physical DMX

Подтверждённый production physical profile:

```text
kDmxMaxChannels       = 512
kDmxPhysicalMaxSlots  = 300
kDmxOutputRefreshHz   = 44
```

На acceptance WB8 подтверждены:

- standard DMX512 wire format;
- Start Code `0x00`;
- continuous fixed 44 Hz output;
- whole snapshot publication;
- serial close/reopen/recovery;
- fast transport: manual DE + hardware BREAK + physical TEMT;
- no custom kernel/WBEC patch required.

Acceptance hardware:

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

## Fixture / Group / Scene / MQTT

Подтверждены:

- RGBW Fixture model и addressing;
- stable monotonic IDs;
- persistence;
- Group membership и factual Group Power;
- Scene create/overwrite/apply/rename/delete;
- one whole post-mutation MQTT DMX snapshot;
- retained cleanup;
- `libmosquitto` transport;
- short callback: parse/enqueue;
- reconnect/resubscribe/full retained republish;
- broker loss не останавливает physical DMX;
- persisted and retained Source:
  - `mqtt`
  - `artnet`.

Persistence остаётся source of truth:

```text
disk
-> C++ canonical model
-> MQTT retained representation
```

## Art-Net 4

Protocol/runtime baseline:

```text
IPv4 UDP port:            6454
minimum protocol revision: 14
Port-Address range:       0..32767
default/compatibility:    0
ArtDmx Length:            even 2..512
network state:            512 channels
physical projection:      channels 1..300
source identity:          IPv4 + Physical
source LOST timeout:      3 s
ArtSync timeout:          4 s
PollReply RefreshRate:    44
PollReply delay:          randomized 0..1 s
```

Поддерживаются:

```text
ArtDmx
ArtPoll
ArtPollReply
ArtSync
```

Confirmed DEV-010 behaviour:

- real UDP 6454 receive/send on WB8;
- QLC+ 5.2.2 discovery/subscription;
- randomized unicast ArtPollReply;
- Targeted Mode filtering;
- subscription remains advertised while Source=MQTT;
- GoodOutput/data-active follows actual Art-Net physical selection;
- 3 s LOST + Hold Last + stale lock release;
- same-process source recovery;
- WB interface down/up recovery;
- controller restart recovery;
- real source IPv4 change across two network interfaces;
- IP+Physical conflict detection with no merge;
- real UDP ArtSync staging and atomic release;
- repeated reconnect;
- high-rate latest/no-FIFO stress;
- fixed physical 44 Hz throughout acceptance.

Acceptance reports:

```text
docs/DEV010A_ARTNET_QLCPLUS_NETWORK_REPORT.txt
docs/DEV010B_ARTNET_MQTT_SOURCE_SWITCH_REPORT.txt
docs/DEV010B_ARTNET_LOST_HOLD_RECOVERY_REPORT.txt
docs/DEV010C1_WB8_INTERFACE_RECOVERY_REPORT.txt
docs/DEV010C2_QLCPLUS_RESTART_RECOVERY_REPORT.txt
docs/DEV010C3_ARTNET_SOURCE_IP_CHANGE_REPORT.txt
docs/DEV010C4_ARTNET_CONFLICT_REPORT.txt
docs/DEV010C5_ARTSYNC_REPORT.txt
docs/DEV010C6_RECONNECT_LATEST_REPORT.txt
```

Reusable protocol/runtime notes:

[`docs/reference/ARTNET4_INTEGRATION.md`](docs/reference/ARTNET4_INTEGRATION.md)

## Art-Net attribution and OEM identity

Art-Net is owned and copyrighted by Artistic Licence.

Required user-guide credit:

> Art-Net™ Designed by and Copyright Artistic Licence Engineering Ltd

Official protocol/OEM resource:

```text
https://art-net.org.uk/
```

DMXWB development acceptance uses an explicitly marked non-production OEM
placeholder. A real registered OEM Code is still required before production
distribution; DMXWB does not invent one.

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

## Что ещё не реализовано / не закрыто

Следующие roadmap gates остаются впереди:

- `DEV-011` — static MQTT-only Web UI;
- `DEV-012` — systemd, diagnostics and fully offline deployment;
- `DEV-013` — full integration, offline install and 24h acceptance.

Production Art-Net OEM registration также остаётся Deferred до production
distribution.

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
