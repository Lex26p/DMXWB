# DMXWB

DMXWB — C++20-приложение, расширяющее контроллеры **серии Wiren Board 8 (WB8)** физическим DMX512 и будущим Art-Net/WB MQTT управлением.

DMXWB не заменяет Wiren Board и использует штатную Linux/MQTT/systemd/web инфраструктуру согласно `docs/TECHNICAL_SPEC.md`. Production artifacts собираются на ноутбуке; Docker не используется.

## Статус

Последний завершённый engineering gate:

```text
DEV-006 — configuration and persistence
80be996746ae87c99563e852c63c0c03a7aa37d1
```

Подтверждены:

- RGBW Fixture model и physical addressing из DEV-005;
- канонические `config.json` / `state.json`;
- persistence schema `version = 1` и config `revision`;
- monotonic `fixture/group/scene` ID counters;
- JSON parse/serialize и полная validation до apply;
- восстановление stable Fixture IDs и logical state после restart;
- atomic config/state write через temporary file + `fsync` + `rename`;
- atomic config transaction с `expected_revision`;
- corrupt config -> safe defaults без перезаписи повреждённого файла;
- corrupt state -> рабочий config + safe Fixture state;
- state dirty scheduling: 2 s debounce и максимум 10 s continuous dirty interval;
- forced dirty-state flush для graceful shutdown;
- persistence runtime отделён от `DmxOutput` и не выполняет file I/O в DMX thread.

DEV-006 host/integration validation:

```text
dmxwb.unit                 PASS
dmxwb.persistence          PASS
dmxwb.persistence_storage  PASS
dmxwb.persistence_runtime  PASS
```

Bullseye ARM64 compatibility build:

```text
Compiler:       aarch64-linux-gnu-g++ 10.2.1
Architecture:   AArch64
Max glibc:      GLIBC_2.17
Dependencies:   libpthread.so.0, libm.so.6, libc.so.6
Artifact SHA256:
01b9d3e4026f639135e1dea50b64cdba7c8150e95fe2b7c6193a633b3486e4d2
```

Artifact SHA совпадает с DEV-006A/006B build: текущий diagnostic `main.cpp` ещё не вызывает `PersistenceRuntime`, поэтому linker не включает этот runtime API в executable. Сам persistence runtime подтверждён отдельным C++ integration test и Bullseye GCC10 compile.

Следующий gate:

```text
DEV-007 — MQTT system and Fixture integration
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

Stable Fixture ID не зависит от DMX-адреса или Name, не переиспользуется после удаления и с DEV-006 переживает restart через persistence.

## Persistence

Канонические runtime paths:

```text
/etc/dmxwb/config.json
/var/lib/dmxwb/state.json
```

`config.json` хранит структурную конфигурацию, revision и monotonic ID counters. `state.json` хранит Source и сохранённое logical Fixture state.

Новый config сначала полностью парсится и валидируется. При transaction проверяется `expected_revision`; рабочая in-memory configuration заменяется только после успешного atomic disk commit.

Runtime state записывается асинхронным persistence-контекстом:

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

До соответствующих roadmap gates намеренно отсутствуют MQTT runtime/integration, полноценные Groups/Scenes operations, Art-Net runtime/parser, production systemd service и Web UI.

Persistence data model/storage/runtime реализованы и integration-confirmed в DEV-006. Production Controller/MQTT wiring начинается в DEV-007.

## Источник истины

Перед каждым шагом читать `AGENTS.md`, `docs/PROJECT_STATE.md`, `docs/TECHNICAL_SPEC.md`, `docs/ROADMAP.md`.
