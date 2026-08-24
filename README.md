# DMXWB

DMXWB — C++20-приложение, расширяющее контроллеры **серии Wiren Board 8 (WB8)** физическим DMX512 и будущим Art-Net/WB MQTT управлением.

DMXWB не заменяет Wiren Board и использует штатную Linux/MQTT/systemd/web инфраструктуру согласно `docs/TECHNICAL_SPEC.md`. Production artifacts собираются на ноутбуке; Docker не используется.

## Статус

Последний завершённый engineering gate:

```text
DEV-005 — Fixture RGBW model and addressing
8a6d6212179a85b880a5eed291afee30bffa6ba0
```

Подтверждены:

- RGBW Fixture model со stable monotonic ID и Name;
- requested/factual Power;
- RGB/Color takeover с `W=0`;
- Temperature `0..100` -> `RGB=255`, `W=0..255`;
- Brightness scaling всех четырёх каналов;
- Power OFF/ON restore;
- Reset;
- Fixture Count / Start Address;
- sequential 4-channel RGBW addressing;
- physical Fixture range только до slot 300;
- immutable whole `DmxSnapshot` из actual Fixture state;
- физический WB8 smoke через `FixtureCollection -> DmxSnapshot -> DmxOutput -> RS-485`.

DEV-005 ARM64 artifact:

```text
SHA256: ef595ec643c419254c6a9395a1c4f47c7b456e5e697872cbe217c5ab075ca30b
```

Hardware marker:

```text
=== DMXWB DEV-005 FIXTURE RGBW HARDWARE PASS ===
```

Report: `docs/DEV005_FIXTURE_HARDWARE_REPORT.txt`.

Следующий gate:

```text
DEV-006 — configuration and persistence
```

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

Stable Fixture ID не зависит от DMX-адреса или Name и не переиспользуется после удаления.

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

При close/stop/error исходная `serial_rs485` конфигурация восстанавливается. Legacy DEV-003 transport остаётся низкоуровневым compatibility fallback, но подтверждённый production profile `<=300 slots / 44 Hz` основан на fast path.

Custom kernel patch на acceptance WB8 не требуется.

## Art-Net contract, зафиксированный до DEV-009

По актуальной Art-Net 4 specification:

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

Отдельные reusable-документы сохраняют технические знания проекта и могут использоваться как база для других разработок:

- [`docs/reference/WB8_RS485_DMX.md`](docs/reference/WB8_RS485_DMX.md) — исследование и реализация физического DMX512 через встроенный RS-485 WB8: transport, DE/BREAK/TEMT, timing, failure cases и hardware acceptance;
- [`docs/reference/ARTNET4_INTEGRATION.md`](docs/reference/ARTNET4_INTEGRATION.md) — Art-Net 4 integration contract: ArtDmx, discovery/subscription, ArtSync, Sequence, conflict, Hold Last и связь network cadence с physical DMX.

`docs/reference/` не заменяет `TECHNICAL_SPEC.md`: reference объясняет инженерные решения и исследования, а нормативные требования конкретно к DMXWB остаются в проектных документах.

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
```

На текущем стенде `/dev/ttyRS485-1` постоянно отключён в WB Serial Device Driver Configuration; hardware helpers считают порт освобождённым и не должны каждый раз спрашивать `s/p/q`.

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

До соответствующих roadmap gates намеренно отсутствуют persistence, MQTT runtime, Groups/Scenes, Art-Net runtime/parser, production systemd service и Web UI.

Fixture model и её physical mapping реализованы и hardware-confirmed в DEV-005; persistence для stable IDs/state начинается в DEV-006.

## Источник истины

Перед каждым шагом читать `AGENTS.md`, `docs/PROJECT_STATE.md`, `docs/TECHNICAL_SPEC.md`, `docs/ROADMAP.md`.
