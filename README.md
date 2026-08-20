# DMXWB

DMXWB — C++20-приложение, расширяющее контроллеры **серии Wiren Board 8 (WB8)** физическим DMX512 и будущим Art-Net/WB MQTT управлением.

DMXWB не заменяет Wiren Board и использует штатную Linux/MQTT/systemd/web инфраструктуру согласно `docs/TECHNICAL_SPEC.md`. Production artifacts собираются на ноутбуке; Docker не используется.

## Статус

Последний завершённый engineering gate:

```text
DEV-004 — continuous DMX engine, timing and serial recovery
b6038b87257d50428de4875308ee3025fcf9ab57
```

Перед DEV-005 зафиксирован подтверждённый hardware follow-up для упрощённого production profile:

```text
physical DMX slots: 1..300 maximum
physical DMX refresh: fixed 44 Hz
```

До изменения core два последовательных production hardware run подтвердили `300 slots / 44 Hz / 60 s`: оба раза `2640/2640`, `missed_deadlines=0`, без видимого flicker; worst observed send time был `17.689 ms` при периоде `22.727 ms`.

После фиксации profile новый ARM64 artifact `670036f5...` прошёл отдельный 60-секундный production acceptance: `2640/2640`, `missed_deadlines=0`, `open/send/recoveries=0`, `active_refresh_hz=44`, `max_send_us=16.407 ms`, visual PASS и final all-off/reopen PASS.

Следующий gate после фиксации этого profile:

```text
DEV-005 — Fixture RGBW model and addressing
```

## Физический DMX profile

Протокол на проводе остаётся стандартным **DMX512**. Число `300` — продуктовый лимит DMXWB, а не новый протокол.

- внутренние DMX/Art-Net структуры сохраняют ёмкость 512 каналов;
- физический RS-485 output принимает максимум 300 slots;
- production cadence фиксирован на 44 Hz;
- отдельной пользовательской настройки Refresh Rate больше нет;
- абсолютный scheduler остаётся `T0`, `T0+period`, ...;
- whole snapshot меняется только на границе кадров;
- `missed_deadlines` остаётся диагностикой фактической способности target выдерживать profile.

Для RGBW при Start Address = 1 максимум физического profile — 75 приборов; требуемые 60 RGBW занимают 240 slots.

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
- ArtPollReply продолжает рекламировать настроенный output universe даже при Source=MQTT, чтобы Art-Net subscription оставалась активной;
- production release требует зарегистрированный Art-Net OEM Code и обязательный Art-Net credit в user documentation.

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

На текущем стенде `/dev/ttyRS485-1` постоянно отключён в WB Serial Device Driver Configuration; hardware helpers могут считать порт освобождённым и не должны каждый раз спрашивать `s/p/q`.

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

До соответствующих roadmap gates намеренно отсутствуют Fixture model, persistence, MQTT runtime, Groups/Scenes, Art-Net runtime/parser, production systemd service и Web UI.

## Источник истины

Перед каждым шагом читать `AGENTS.md`, `docs/PROJECT_STATE.md`, `docs/TECHNICAL_SPEC.md`, `docs/ROADMAP.md`.
