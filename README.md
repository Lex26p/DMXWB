# DMXWB

DMXWB — C++20-приложение, расширяющее контроллеры **серии Wiren Board 8 (WB8)**
физическим DMX512, WB MQTT-интеграцией, Art-Net управлением и локальным
MQTT-based Web.

DMXWB не заменяет Wiren Board и не является самостоятельной SCADA. Приложение
использует штатную Linux/MQTT/systemd/nginx инфраструктуру WB согласно
[`docs/TECHNICAL_SPEC.md`](docs/TECHNICAL_SPEC.md). Docker не используется.

## Статус

Последний подтверждённый engineering step:

```text
DEV-013 — full integration and final acceptance
```

DEV-013A–D подтвердили финальную host/ARM64-сборку, Fixture/Group/Scene/MQTT,
Art-Net/Source/Web/recovery и полностью офлайн-установленный релиз после reboot
реального WB8. Исходная среда контроллера после acceptance восстановлена.

Текущая точка процесса:

```text
DEV-014C — clean-state acceptance and replacement final package
```

DEV-014A и DEV-014B получили focused PASS. DEV-014C host/ARM64 PASS собрал
заменяющий пакет `0.1.1` с `source_id=dev014c-final`. До окончательного
релиза осталась одна clean-state/reboot проверка этого пакета на WB8.

Актуальная подробная точка проекта:
[`docs/PROJECT_STATE.md`](docs/PROJECT_STATE.md).

## Инструкции

- [`docs/WEB_USER_GUIDE.md`](docs/WEB_USER_GUIDE.md) — подключение, Source,
  светильники, группы, сцены, настройки и восстановление Web-связи.
- [`docs/INSTALL_UPDATE_REMOVE_GUIDE.md`](docs/INSTALL_UPDATE_REMOVE_GUIDE.md) —
  пошаговая офлайн-установка, обновление, обычное удаление с сохранением данных,
  повторная установка, явный purge, проверки и полный справочник команд.

## Подтверждённая архитектура

```text
Browser
/dmxwb/
    |
MQTT WebSocket /mqtt
    |
Mosquitto
    |
    +---------------- WB MQTT ----------------+
    |                                         |
Fixture / Group / Scene                Art-Net UDP 6454
    |                                         |
mqtt whole snapshot                    latest Art-Net snapshot
    |                                         |
    +--------------- DmxSourceRouter ---------+
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
- Web/MQTT/Art-Net не пишут в serial напрямую;
- Source выбирается только явно: `mqtt | artnet`;
- автоматического source switching нет;
- оба источника продолжают обновляться в background;
- только whole snapshots доходят до физического DMX;
- Art-Net LOST использует Hold Last;
- physical output работает фиксированно на 44 Hz;
- Web не нужен для продолжения DMX после закрытия браузера.

## Physical DMX

Подтверждённый product profile:

```text
kDmxMaxChannels       = 512
kDmxPhysicalMaxSlots  = 300
kDmxOutputRefreshHz   = 44
```

На WB8 подтверждены continuous DMX512, Start Code `0x00`, whole-snapshot
publication, serial recovery и fast transport с manual DE/BREAK/TEMT без custom
kernel/WBEC patch.

DEV-011 дополнительно подтвердил runtime-переключение физического порта:

```text
/dev/ttyRS485-1 -> /dev/ttyRS485-2 -> /dev/ttyRS485-1
```

в одном процессе без нарушения 44 Hz.

## Fixture / Group / Scene / MQTT

Подтверждены:

- RGBW Fixture model и addressing;
- stable monotonic IDs;
- persistence;
- Group membership и factual Group state;
- Scene create/apply/overwrite/rename/delete;
- atomic whole-snapshot Scene Apply;
- retained cleanup;
- `libmosquitto` transport;
- short callback: parse/enqueue;
- reconnect/resubscribe/full retained republish;
- persisted Source `mqtt | artnet`;
- broker/browser outages не останавливают physical DMX.

Canonical state flow:

```text
disk
-> C++ canonical model
-> retained MQTT representation
```

## Static Web

Production Web:

```text
www/dmxwb/
    index.html
    app.js
    model.js
    mqtt-client.js
    styles.css
```

Свойства:

- static HTML/CSS/vanilla JS;
- no Node.js runtime;
- no npm/build step;
- no external Internet dependencies;
- URL `/dmxwb/`;
- MQTT WebSocket `/mqtt`;
- русский пользовательский интерфейс;
- Fixture/Group/Scene/Source controls;
- structural Settings с локальным draft и явным `Применить`;
- two-tab revision conflict protection;
- reconnect без повторной отправки старых команд;
- только MQTT API — без direct serial/file/systemd access.

Structural settings, включая `DMX Port` и `Art-Net Universe`, подтверждены на
реальном WB8 и применяются running process без restart.

`/dmxwb/status` в DEV-011 намеренно минимален и содержит только обязательные
верхнеуровневые состояния. Расширенные deployment/service diagnostics относятся к
DEV-012.

## Art-Net 4

Поддерживаются:

```text
ArtDmx
ArtPoll
ArtPollReply
ArtSync
```

Основной подтверждённый contract:

```text
IPv4 UDP port:             6454
minimum protocol revision: 14
Port-Address range:        0..32767
compatibility default:     0
network state:             512 channels
physical projection:       channels 1..300
source identity:           IPv4 + Physical
source LOST timeout:       3 s
ArtSync timeout:           4 s
PollReply RefreshRate:     44
```

DEV-010 подтвердил real UDP, QLC+ discovery, ArtSync, source loss/recovery,
interface/controller recovery, source IPv4 change, conflict/no-merge и
latest-state/no-FIFO behaviour.

DEV-011 подтвердил runtime Art-Net Port-Address:

```text
0 -> 17 -> 0
```

с отбрасыванием старого universe cache и Hold Last без blackout.

Подробности:
[`docs/reference/ARTNET4_INTEGRATION.md`](docs/reference/ARTNET4_INTEGRATION.md).

## Acceptance hardware

Primary tested configuration:

```text
Controller:       Wiren Board rev. 8.5.1 (T507)
Architecture:     arm64
OS:               Debian 11 Bullseye
WB release:       wb-2606 stable
Kernel:           6.8.0-wb160
glibc:            2.31
DMX ports:        /dev/ttyRS485-1, /dev/ttyRS485-2
MQTT broker:      127.0.0.1:1883
```

## Build/test среда

Windows/MSVC не входит в поддерживаемую build/test matrix.

```text
Windows host                 -> project files, ZIP, Git, WSL launch
Local Linux / WSL on laptop -> native C++ build/tests + WB8 target build
WB8                          -> runtime/hardware/integration acceptance
Docker                       -> not used
```

Native Linux/WSL:

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

## Что ещё впереди

```text
DEV-013  — full integration and final acceptance
```

До production distribution также остаётся Deferred зарегистрированный Art-Net OEM
Code. Development acceptance не подменяет его выдуманным production code.

## Инженерные reference-документы

- [`docs/reference/WB8_RS485_DMX.md`](docs/reference/WB8_RS485_DMX.md)
- [`docs/reference/ARTNET4_INTEGRATION.md`](docs/reference/ARTNET4_INTEGRATION.md)

`docs/reference/` не заменяет normative `TECHNICAL_SPEC.md`,
`docs/ROADMAP.md` и `docs/PROJECT_STATE.md`.

## Источник истины

Перед каждым шагом читать:

```text
AGENTS.md
docs/PROJECT_STATE.md
docs/TECHNICAL_SPEC.md
docs/ROADMAP.md
```
