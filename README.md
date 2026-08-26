# DMXWB

DMXWB — C++20-приложение, расширяющее контроллеры **серии Wiren Board 8 (WB8)** физическим DMX512, WB MQTT-интеграцией и будущим Art-Net управлением.

DMXWB не заменяет Wiren Board и использует штатную Linux/MQTT/systemd/web инфраструктуру согласно `docs/TECHNICAL_SPEC.md`. Production artifacts собираются на ноутбуке; Docker не используется.

## Статус

Последний завершённый engineering gate:

```text
DEV-007 — MQTT system and Fixture integration
a23f0d6628c8f18ecbb51be0a8a5b02b770a4bd8
```

Подтверждены:

- physical DMX512 output через встроенный RS-485 WB8;
- fixed physical profile `<=300 slots / 44 Hz`;
- RGBW Fixture model, stable IDs и whole DMX snapshots;
- canonical config/state persistence из DEV-006;
- `libmosquitto` transport для localhost broker `127.0.0.1:1883`;
- короткий MQTT callback: parse/enqueue без serial/file I/O;
- FIFO Controller command processing;
- system device `dmxwb` с `status` и `source`;
- Fixture MQTT controls `name/power/red/green/blue/color/brightness/temperature/reset`;
- Fixture metadata `hidden=true`, чтобы они не засоряли штатный WB web;
- retained state/metadata и non-retained command contract;
- retained `/on` commands игнорируются;
- reconnect/resubscribe + full retained republish;
- retained `status=off` при graceful stop и MQTT LWT `off`;
- disk -> C++ model -> MQTT остаётся source-of-truth направлением;
- MQTT loss/recovery не останавливает continuous physical DMX;
- `/dmxwb/config/set` + `/dmxwb/config/result`;
- config request envelope с `request_id`, `expected_revision` и полной proposed config;
- config API переиспользует DEV-006 atomic transaction/validation;
- stale/invalid config не меняет рабочую disk/in-memory model;
- удалённые Fixture IDs очищают старые retained MQTT topics.

DEV-007 host validation:

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

Bullseye ARM64 compatibility:

```text
Compiler:              aarch64-linux-gnu-g++ 10.2.1
Cross libmosquitto:    2.0.11
Architecture:          AArch64
Max glibc:             GLIBC_2.17
```

Текущий diagnostic executable:

```text
artifacts/wb8-bullseye-arm64/dmxwb
SHA256:
01b9d3e4026f639135e1dea50b64cdba7c8150e95fe2b7c6193a633b3486e4d2
```

Он по-прежнему не является production MQTT daemon entrypoint, поэтому linker не включает MQTT runtime в этот diagnostic binary.

Отдельный DEV-007 acceptance runtime:

```text
artifacts/wb8-bullseye-arm64/dmxwb-mqtt-acceptance
NEEDED: libmosquitto.so.1
Latest DEV-007C cross-build SHA256:
ba7363035897265295cac3d3af00dd0ebe6abd86ad5efa33f278acec1767b6b8
```

Физический DEV-007B hardware acceptance был выполнен на WB8 до DEV-007C и зафиксирован в:

```text
docs/DEV007_MQTT_HARDWARE_REPORT.txt
=== DMXWB DEV-007 MQTT + FIXTURE HARDWARE PASS ===
```

Следующий gate:

```text
DEV-008 — Groups and Scenes
```

## MQTT contract

Broker на WB8:

```text
127.0.0.1:1883
```

Системное устройство:

```text
/devices/dmxwb/controls/status
/devices/dmxwb/controls/source
/devices/dmxwb/controls/source/on
```

Fixture device:

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

State и metadata публикуются retained. Команды `/on` принимаются только non-retained; retained commands игнорируются.

После reconnect выполняются resubscribe и полная retained републикация актуального состояния. При broker loss физический `DmxOutput` продолжает работать независимо от MQTT network loop.

MQTT callback только разбирает transport message и кладёт команду в очередь. Из callback не выполняются:

```text
Fixture model mutation
persistence file I/O
serial I/O
DmxOutput operations
```

Эти действия выполняются Controller/runtime context.

## MQTT configuration API

Каноническая config state:

```text
/dmxwb/config
```

Изменение:

```text
/dmxwb/config/set
```

Минимальный envelope:

```json
{
  "request_id": "opaque-token",
  "expected_revision": 7,
  "config": {
    "...": "complete canonical AppConfig"
  }
}
```

Result:

```text
/dmxwb/config/result
```

Содержит:

```text
request_id
ok
revision
error_code
message
```

Новый config проходит тот же DEV-006 validation/atomic storage path. Ошибка schema/version/revision/disk commit не заменяет уже работающую configuration.

## Физический DMX profile

Протокол на проводе остаётся стандартным **DMX512**. Число `300` — продуктовый лимит DMXWB, а не новый протокол.

- внутренние DMX/Art-Net структуры сохраняют ёмкость 512 каналов;
- физический RS-485 output принимает максимум 300 slots;
- production cadence фиксирован на 44 Hz;
- отдельной пользовательской настройки Refresh Rate нет;
- абсолютный scheduler остаётся `T0`, `T0+period`, ...;
- whole snapshot меняется только на границе кадров;
- `missed_deadlines` остаётся диагностикой фактической способности target выдерживать profile.

Для RGBW при Start Address = 1 максимум physical profile — 75 приборов; требуемые 60 RGBW занимают 240 slots.

## Fixture RGBW model

Каждый Fixture занимает четыре последовательных канала:

```text
Address + 0 = R
Address + 1 = G
Address + 2 = B
Address + 3 = W
```

Новый Fixture создаётся OFF, но сохраняет `RGBW=255/255/255/255`, `Brightness=100`, `Temperature=100`.

Основные semantics:

```text
RGB/Color       -> W = 0
Temperature 0   -> 255/255/255/0
Temperature 50  -> 255/255/255/128
Temperature 100 -> 255/255/255/255
Brightness      -> saved_channel * percent / 100
Power OFF       -> actual 0/0/0/0, saved state сохраняется
Power ON        -> saved state восстанавливается через текущий Brightness
Reset           -> ON, Brightness 100, Temperature 100, RGBW 255/255/255/255
```

Stable Fixture ID не зависит от DMX-адреса или Name, не переиспользуется после удаления и переживает restart через persistence.

## Persistence

Канонические runtime paths:

```text
/etc/dmxwb/config.json
/var/lib/dmxwb/state.json
```

`config.json` хранит структурную конфигурацию, revision и monotonic ID counters. `state.json` хранит Source и сохранённое logical Fixture state.

Новый config сначала полностью парсится и валидируется. При transaction проверяется `expected_revision`; рабочая in-memory configuration заменяется только после успешного atomic disk commit.

Runtime state записывается:

```text
last change + 2 s
OR
first dirty + 10 s
```

Ошибка записи оставляет state dirty для следующей попытки. Graceful shutdown использует forced flush.

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

При close/stop/error исходная `serial_rs485` конфигурация восстанавливается. Legacy DEV-003 transport остаётся compatibility fallback, но подтверждённый production profile `<=300 slots / 44 Hz` основан на fast path.

Custom kernel patch на acceptance WB8 не требуется.

## Art-Net contract, зафиксированный до DEV-009

- UDP 6454, IPv4;
- ArtDmx data length — even `2..512`;
- внутренний `artnet_state[512]` сохраняется полностью;
- физически используются только channels `1..300`;
- `ArtPollReply.RefreshRate = 44`;
- ArtDmx не ставится в FIFO: latest snapshot wins;
- физический DMX clock независим и всегда 44 Hz;
- `ArtSync` поддерживается через staging snapshot и release по следующему ArtSync; timeout обратно в async mode — 4 s;
- source identity для конфликта — `source IP + ArtDmx.Physical`;
- multiple-source policy DMXWB = `CONFLICT`, без HTP/LTP merge;
- ArtPollReply продолжает рекламировать настроенный output universe даже при Source=MQTT;
- production release требует зарегистрированный Art-Net OEM Code и обязательный Art-Net credit в user documentation.

## Инженерные reference-документы

- [`docs/reference/WB8_RS485_DMX.md`](docs/reference/WB8_RS485_DMX.md) — physical DMX512 через встроенный RS-485 WB8;
- [`docs/reference/ARTNET4_INTEGRATION.md`](docs/reference/ARTNET4_INTEGRATION.md) — Art-Net 4 integration contract.

`docs/reference/` не заменяет `TECHNICAL_SPEC.md`.

## Hardware acceptance target

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

На текущем стенде `/dev/ttyRS485-1` постоянно отключён в WB Serial Device Driver Configuration; hardware helpers считают порт освобождённым и не останавливают `wb-mqtt-serial` без отдельной необходимости.

## Build/test среда

C++ build/test поддерживается только на Linux. Windows/MSVC не входит в build/test matrix.

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

- полноценная Group/Scene runtime logic и MQTT contract;
- Art-Net runtime/parser/source integration;
- собственный Web UI;
- production systemd daemon lifecycle и offline deployment bundle;
- final 24h acceptance.

MQTT system + Fixture integration и structural config API реализованы и подтверждены в DEV-007. Отдельный `dmxwb-mqtt-acceptance` остаётся engineering acceptance runtime, а production daemon/service wiring выполняется позже по roadmap.

## Источник истины

Перед каждым шагом читать `AGENTS.md`, `docs/PROJECT_STATE.md`, `docs/TECHNICAL_SPEC.md`, `docs/ROADMAP.md`.
