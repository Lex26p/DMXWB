# DMXWB DEVELOPMENT ROADMAP

**Статус:** рабочая дорожная карта реализации утверждённого `TECHNICAL_SPEC.md`  
**База актуализации дорожной карты:** `0c4e9f2ea0c06785d0a3ce5c714d46a0ef29ab4d`  
**Дата актуализации:** 2026-09-03  
**Цель:** последовательно реализовать полное приложение DMXWB с проверяемым PASS/FAIL после каждого значимого engineering gate.

Дорожная карта является рабочей инструкцией по порядку разработки. Источник истины — актуальное состояние репозитория. Перед каждым шагом необходимо прочитать `AGENTS.md`, `PROJECT_STATE.md`, относящиеся к задаче части `TECHNICAL_SPEC.md` и настоящий roadmap.

Пользователь самостоятельно вносит подготовленные изменения в репозиторий. Новый полный SHA после пользовательского commit/push подтверждает завершение конкретного шага и разрешает переход к следующему шагу, если он предусмотрен. Engineering gate получает PASS только по своим фактическим критериям — один только documentation commit или наличие кода в `master` не заменяет hardware/integration test.

Текущий продуктовый приоритет: физический DMX является ядром системы; Art-Net — полноценный внешний источник того же DMX-выхода; MQTT/Web — средства локального управления и интеграции с Wiren Board; production installation bundle устанавливается полностью офлайн.

Целевая платформа — **контроллеры серии Wiren Board 8 (WB8)**. DMXWB является расширением возможностей Wiren Board, а не заменой контроллера или самостоятельной SCADA.

Development/build environment:

```text
Windows host                 -> project files / editor / ZIP / Git / WSL launch
Local Linux / WSL on laptop -> all C++ host build/tests + target build
WB8                          -> runtime/hardware/integration acceptance
Docker                       -> не используется
```

Windows compiler/MSVC не входит в поддерживаемую build/test matrix. C++ build и tests выполняются только в Linux/WSL; target artifact собирается Bullseye ARM64 cross-toolchain.

---

## 1. Правила дорожной карты

Дорожная карта описывает порядок разработки, но не заменяет техническое задание.

При конфликте документов по продуктовым требованиям приоритет:

1. `docs/TECHNICAL_SPEC.md` — что должно быть реализовано.
2. `docs/ROADMAP.md` — в каком порядке это реализуется и проверяется.
3. `docs/PROJECT_STATE.md` — где находится текущая работа и что уже подтверждено.

Процесс взаимодействия и формат handoff определяет `AGENTS.md`.

Каждый engineering gate:

- имеет одну основную инженерную цель;
- выполняется от актуального состояния репозитория;
- включает код, тесты и относящуюся к этапу документацию;
- не считается завершённым только потому, что код компилируется;
- получает PASS только после предусмотренных automated/hardware/integration tests;
- при FAIL остаётся текущим gate до устранения причины;
- не расширяется задачами будущих gates без отдельной причины и изменения roadmap.

Между engineering gates и внутри текущего gate допускаются отдельные документационные, организационные и build-enablement шаги. Такой шаг завершается пользовательским commit SHA, но сам по себе не означает PASS текущего engineering gate.

Перед каждым новым шагом ассистент заново сверяется с актуальным репозиторием. SHA из предыдущего handoff используется для трассируемости и подтверждения завершения шага, а не вместо чтения репозитория.

Разработка идёт к одному приложению. Этапы не являются отдельными продуктовыми версиями.

### 1.1. Platform/build rules

- target = серия WB8, а не одна фиксированная версия контроллера;
- конкретная модель WB8 и версия WB software фиксируются в hardware acceptance;
- host build/tests выполняются на ноутбуке пользователя в Linux/WSL;
- Windows используется для project files/editor/ZIP/Git и запуска WSL, но Windows compiler/MSVC не поддерживается;
- GNU C++ и Clang на Linux являются поддерживаемыми host compilers;
- локальный Linux/WSL используется для всех C++ host tests и target build;
- production binary не компилируется на WB8 как обязательная часть workflow;
- конкретный non-Docker target toolchain должен быть практически подтверждён до production deployment;
- Docker не используется для build, cross-build, runtime, deployment или обязательных tests.

---

## 2. Основной порядок

```text
DEV-001  Foundation / build / tests
    |
DEV-002  DMX core types and deterministic frame model
    |
DEV-003  Physical DMX transport proof on /dev/ttyRS485-1
    |
DEV-004  Continuous DMX engine, timing and serial recovery
    |
DEV-005  Fixture RGBW model and addressing
    |
DEV-006  Configuration and persistence
    |
DEV-007  MQTT system + Fixture integration
    |
DEV-008  Groups and Scenes
    |
DEV-009  Art-Net protocol core
    |
DEV-010  Art-Net runtime, recovery and Source switching
    |
DEV-011  Static MQTT-only Web UI
    |
DEV-012  systemd, diagnostics and fully offline deployment
    |
DEV-013  Full integration and final acceptance
```

### 2.1. Краткая карта gates

| Gate | Что делаем | Где подтверждаем PASS |
|---|---|---|
| `DEV-001` | C++20/CMake foundation и test harness | Windows / Visual Studio 2026 |
| `DEV-002` | Immutable DMX snapshots, channel/slot model, frame core | Host unit tests |
| `DEV-003` | Target build path + минимальный физический DMX transport | Local Linux + реальный WB8 + светильник |
| `DEV-004` | Непрерывный DMX engine, timing, refresh, serial recovery | Host + WB8 |
| `DEV-005` | RGBW Fixture model, addressing, stable IDs | Host + WB8 DMX smoke |
| `DEV-006` | Config/state persistence и атомарные транзакции | Host/integration tests |
| `DEV-007` | WB MQTT system + Fixture contract | Host/WB8 MQTT + DMX |
| `DEV-008` | Groups и Scenes | Host + несколько DMX addresses |
| `DEV-009` | Art-Net protocol parser/state machine | Host unit tests |
| `DEV-010` | Art-Net UDP runtime, recovery, source switching | WB8 + внешний Art-Net source |
| `DEV-011` | Статический MQTT-only Web UI | Browser + локальный MQTT/WB8 |
| `DEV-012` | systemd, diagnostics и offline deployment bundle | Чистая offline-установка на WB8 |
| `DEV-013` | Полная функциональная финальная acceptance | Реальная WB8-система целиком |

Критический путь:

```text
C++ foundation
    -> physical DMX
    -> stable continuous DMX
    -> application logic
    -> MQTT
    -> Art-Net reliability
    -> Web
    -> production/offline deployment
    -> final acceptance
```

Физический DMX проверяется раньше остальных крупных подсистем, потому что он является основой приложения.

---

# DEV-001 — C++ foundation, build and test harness

## Цель

Создать минимальную воспроизводимую основу C++20-проекта без hardware side effects.

## Реализовать

- корневой `CMakeLists.txt`;
- C++20;
- production executable target `dmxwb`;
- отдельный unit-test target;
- базовую структуру `src/` и `tests/`;
- минимальный `main.cpp`;
- общий namespace проекта;
- warning/error rules;
- deterministic unit-test runner;
- базовые типы ошибок/результатов только при необходимости.

До добавления сторонней runtime-зависимости подтвердить её пригодность для поддерживаемых WB8 и офлайн-deployment.

## Не включать

- serial;
- MQTT;
- Art-Net;
- persistence;
- systemd;
- web;
- полноценную Fixture/Group/Scene модель.

## Tests

На Windows / Visual Studio 2026:

- clean CMake configure;
- clean build;
- unit-test executable запускается;
- simple test PASS;
- production executable запускается без hardware side effects.

При необходимости те же core tests повторяются в локальном Linux.

## PASS

- проект собирается с нуля;
- unit tests PASS;
- нет hardware side effects;
- структура соответствует ТЗ;
- README/PROJECT_STATE содержат фактические команды.

---

# DEV-002 — DMX core types and deterministic frame model

## Цель

Создать hardware-independent ядро данных, которое физический DMX thread получает только целым immutable snapshot.

## Реализовать

- `DmxSnapshot`;
- максимум 512 каналов;
- `slot_count`;
- generation/revision snapshot;
- безопасную one-based индексацию 1..512;
- расчёт slot_count;
- Start Code отдельно от channels;
- immutable/double-buffer publication primitive или эквивалент;
- clock/scheduling helper interface без serial.

## Не включать

- реальный termios;
- BREAK;
- Fixture algorithms;
- MQTT;
- Art-Net.

## Tests

- channel 1 и 512;
- invalid channel rejection;
- slot_count 40/60 examples;
- snapshot не наблюдается частично обновлённым;
- Start Code не создаёт off-by-one;
- generation/data сохраняются целиком.

## PASS

Core tests deterministic и hardware-independent.

---

# DEV-003 — physical DMX transport proof

## Цель

Доказать на реальном контроллере **серии WB8**, что C++ transport физически выдаёт рабочий DMX512 через встроенный `/dev/ttyRS485-1`.

Это первый обязательный hardware gate проекта.

Конкретная модель WB8 и версия установленного ПО фиксируются в `PROJECT_STATE.md` вместе с результатом hardware test, но они не становятся единственной целевой платформой проекта.

## DEV-003A — target build enablement

Перед физическим запуском должен существовать способ получить Linux binary для тестируемого WB8 **на ноутбуке пользователя**.

Используется локальный Linux. Docker не используется.

Нужно определить и подтвердить фактический target build path:

- архитектура тестируемого WB8;
- подходящий compiler/cross compiler;
- sysroot/runtime ABI при необходимости;
- CMake toolchain/configuration при необходимости;
- способ доставки готового binary на WB8;
- запуск `dmxwb --version` на WB8.

Этот подпункт не требует production installer и не является DEV-012. Его задача только дать воспроизводимый бинарник для текущего hardware gate.

## DEV-003B — physical transport proof

Минимальный `DmxTransport`:

- открыть выбранный serial;
- default `/dev/ttyRS485-1`;
- data mode `250000 8N2`;
- software BREAK по WB-подходу:
  - 38400;
  - write `0x00`;
  - wait/drain;
  - возврат 250000 8N2;
- отправить Start Code `0x00`;
- отправить фиксированный RGBW pattern;
- корректно закрыть порт.

Отдельный diagnostic CLI/test mode разрешён и не должен смешиваться с production runtime logic.

## Hardware test

На реальном RGBW fixture с известным стартовым адресом:

- all-off;
- R only;
- G only;
- B only;
- W only;
- all-on.

При наличии безопасного измерительного оборудования можно проверить bit timing/BREAK. Earth-grounded oscilloscope к A/B не подключать без подтверждённой безопасной схемы измерения.

## STOP CONDITION

Если transport не даёт стабильный физический DMX:

- не переходить к Fixture/MQTT/Art-Net;
- не маскировать проблему retries/UI;
- оставаться на DEV-003;
- исправлять transport/build compatibility в рамках gate.

Возврат к custom kernel/WBEC требует отдельного изменения ТЗ.

## PASS

- target binary собран на ноутбуке и запускается на тестируемом WB8;
- fixture устойчиво реагирует на все patterns;
- процесс не требует patch kernel/WBEC;
- serial после теста освобождается;
- model/WB software/build toolchain и hardware result документированы.

---

# DEV-004 — continuous DMX engine, timing and serial recovery

## Цель

Независимый continuous physical output с deterministic whole-frame publication, absolute scheduling и serial recovery.

## Final production profile

После hardware follow-up DEV-004 зафиксирован окончательный physical profile:

```text
DMX512 physical layer
maximum physical slots = 300
fixed output cadence = 44 Hz
```

Core/network data capacity остаётся 512 channels. Пользовательского Refresh Rate нет.

## Реализовано / проверено

- single `DmxOutput` worker;
- absolute frame-start grid;
- whole snapshot only at frame boundary;
- preallocated mailbox;
- close/reopen/retry recovery;
- fast WB8 transport: manual DE + hardware BREAK + physical TEMT;
- legacy DEV-003 low-level fallback;
- diagnostics/missed-deadline accounting;
- full 512/30 proof в раннем DEV-004B;
- 240/44 proof;
- 512/40 one-minute experiment: 1 missed deadline -> rejected as fixed profile;
- two consecutive pre-change 300/44 one-minute runs: 2640/2640, zero missed, no flicker; worst max send 17.689 ms;
- post-change fixed-profile ARM64 artifact `670036f5...`: 300/44 for 60 s, 2640/2640, zero missed/failures, max send 16.407 ms, no flicker, final all-off/reopen PASS.

## PASS

DEV-004 PASS. Post-gate profile decision confirmed: supported production physical range `<=300 slots` at fixed `44 Hz` on the acceptance fast path. Custom kernel patch not required.

# DEV-005 — Fixture RGBW model and addressing

## Цель

Реализовать утверждённую RGBW Fixture model независимо от MQTT/Web.

## Реализовать

Fixture:

- stable ID;
- Name;
- requested Power;
- R/G/B/W;
- Brightness;
- Temperature;
- actual RGBW;
- factual Power;
- RGB/Color -> W=0;
- Temperature -> RGB=255, W=0..255;
- Brightness scaling всех каналов;
- Power restore;
- Reset = ON, Brightness 100, RGBW 255.

Fixture collection/config:

- Fixture Count;
- Start Address;
- 4 channels per Fixture;
- sequential addressing;
- validation physical address <=300;
- Count=0;
- stable monotonic IDs;
- no ID reuse;
- Start Address не меняет logical state;
- один целый mqtt DmxSnapshot.

## Tests

Все алгоритмы TECHNICAL_SPEC, включая RGB takeover, Color, Temperature 0/50/100, Brightness, Power restore, factual Power при Brightness=0, Reset, addressing boundaries, ID reuse protection и atomic snapshot rebuild.

## Hardware smoke

Через доказанный `DmxOutput` проверить R/G/B, Temperature, Brightness, Power OFF/ON restore и Reset.

## PASS

Модель и physical output совпадают с ТЗ.

---

# DEV-006 — configuration and persistence

## Цель

Добавить каноническую конфигурацию и runtime state без влияния file I/O на DMX timing.

## Реализовать

- `/etc/dmxwb/config.json`;
- `/var/lib/dmxwb/state.json`;
- version/revision;
- monotonic fixture/group/scene counters;
- parse/serialize;
- full validation before apply;
- atomic tmp + fsync + rename;
- dirty state;
- 2 s debounce;
- max 10 s dirty interval;
- forced save on graceful shutdown;
- safe defaults;
- corrupt state/config behavior;
- atomic config transaction.

## Не включать

MQTT transport ещё не нужен: API тестируется через C++ helpers.

## Tests

- round trip;
- invalid schema/version;
- invalid fixture range;
- missing Group member;
- Scene with missing Fixture;
- revision mismatch;
- atomic replace failure simulation;
- corrupt state/config fallback;
- debounce;
- stable IDs survive restart.

## PASS

- persistence не вызывается из DMX thread;
- restart восстанавливает state;
- повреждённый новый config не заменяет рабочий.

---

# DEV-007 — MQTT system and Fixture integration

## Цель

Подключить libmosquitto и реализовать MQTT contract системного устройства и Fixtures.

## Реализовать

MQTT lifecycle:

- localhost broker;
- reconnect/subscriptions;
- non-retained commands;
- retained command rejection;
- metadata;
- retained state;
- full republish after reconnect;
- LWT/offline behavior.

System device:

```text
/devices/dmxwb/controls/status
/devices/dmxwb/controls/source
/devices/dmxwb/controls/source/on
```

Fixture controls:

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

Internal/web snapshots:

```text
/dmxwb/config
/dmxwb/state
/dmxwb/status
/dmxwb/config/set
/dmxwb/config/result
```

MQTT callback только создаёт Commands и помещает их в Controller queue.

## Tests

- `/on -> Controller -> snapshot -> state`;
- Color;
- RGB after Brightness/OFF;
- saved state in `/dmxwb/state`;
- retained `/on` ignored;
- reconnect resync;
- broker down/up without process restart;
- Source restored from disk, not broker.

## Hardware integration

Управлять реальным fixture через WB MQTT и подтвердить physical result.

## PASS

MQTT loss/recovery не останавливает DMX loop.

---

# DEV-008 — Groups and Scenes

## Цель

Реализовать Group/Scene logic поверх Fixture/MQTT/persistence.

## Реализовать

Groups:

- stable monotonic ID;
- Name;
- members by Fixture ID;
- multiple group membership;
- все согласованные controls;
- factual Group Power = OR(member Power);
- last group setting state;
- Fixture deletion cleans memberships;
- empty group allowed.

Scenes:

- stable monotonic ID;
- Name;
- create/overwrite/apply/delete;
- Fixture snapshots by stable ID;
- missing Fixture ignored;
- new Fixture untouched;
- Apply не переключает Source;
- one atomic mqtt DMX snapshot.

MQTT devices согласно ТЗ.

## Tests

- multiple groups;
- last command wins;
- group Power restore/reset/empty;
- factual group Power;
- scene lifecycle;
- fixture deletion/addition;
- atomic scene snapshot;
- retained topic cleanup.

## Hardware integration

Проверить Group/Scene на нескольких реальных DMX addresses.

## PASS

Нет последовательного visual перебора Fixtures при Scene Apply.

---

# DEV-009 — Art-Net protocol core

## Цель

Реализовать deterministic Art-Net 4 parser/state machine без real UDP runtime.

## Реализовать

- protocol revision >=14, OpCodes and safe minimum-length parsing;
- ArtDmx / ArtPoll / ArtPollReply / ArtSync;
- one 15-bit Port-Address;
- even ArtDmx Length `2..512`;
- accept mandatory bytes, ignore valid trailing extension bytes;
- persistent `artnet_state[512]`;
- source-to-physical projection channels `1..300`;
- short packet per-channel Hold Last;
- Sequence 0 disabled, non-zero rollover/stale protection without waiting for gaps;
- source identity `IPv4 + Physical`;
- WAITING/ACTIVE/LOST/CONFLICT;
- documented CONFLICT policy, no HTP/LTP merge;
- ArtSync staging/release and 4 s fallback to async;
- ArtPoll Targeted Mode;
- ArtPollReply one output subscription, `RefreshRate=44`, output subscription remains advertised independent of DMXWB Source;
- OEM Code represented as explicit configuration/build identity; no invented production value.

## Tests

Header/opcode/version/minimum length/trailing bytes, Port-Address, 0 compatibility exception, Length, 512 state/300 projection, short packets, Sequence rollover/stale, IP+Physical conflict, ArtSync timing state, Targeted ArtPoll and PollReply fields.

## PASS

Core/state machine deterministic, socket-free and consistent with current official Art-Net 4 specification.

# DEV-010 — Art-Net runtime, reliability and Source switching

## Цель

Подключить Art-Net core к UDP 6454 и доказать recovery/source switching на WB8.

## Реализовать

- IPv4 UDP 6454 bind/rebind;
- receive ArtDmx and ArtSync;
- ArtPoll -> randomized (0..1 s) unicast ArtPollReply;
- Targeted Mode filtering;
- advertise configured `SwOut` subscription even while Source=MQTT;
- `RefreshRate=44`;
- modern unicast subscription behavior; legacy broadcast receive may be accepted for compatibility but is not required for conformance;
- 3 s LOST timeout, Hold Last, socket recovery;
- latest committed snapshot only, no ArtDmx FIFO;
- Source switch MQTT/ART-NET only at physical frame boundary;
- no first ArtDmx -> preserve current physical output until valid snapshot;
- LOST -> Source stays ART-NET, no blackout;
- return -> automatic recovery without restart.

## Acceptance

Use a named/versioned Art-Net controller/tool. Verify discovery/subscription, 44 fps input, ArtSync, Ethernet loss durations, source restart/IP change, WB interface down/up, repeated reconnect and IP+Physical conflict.

## PASS

No operator restart after temporary network failure; no accumulating ArtDmx latency; physical output remains fixed 44 Hz and uses latest committed channels 1..300.

# DEV-011 — static MQTT-only Web UI

## Цель

Реализовать статический интерфейс поверх стабильного MQTT API.

## Реализовать

```text
www/dmxwb/
    index.html
    app.js
    model.js
    mqtt-client.js
    styles.css
```

Без Node.js runtime/build step и без внешних internet dependencies.

Разделы:

- Управление;
- Светильники и группы;
- Сцены;
- Настройки.

Реализовать Fixture controls, constructor, Groups, Scenes, Settings, diagnostics, MQTT reconnect, config revision conflict, slider throttle 20–30/s и final value publish.

## Tests

- offline/static load;
- no Node runtime;
- MQTT reconnect;
- two-tab revision conflict;
- invalid config reject;
- Fixture/Group/Scene;
- Source switching;
- diagnostics.

## PASS

Web не имеет прямого serial/file/systemd API и не нужен для продолжения DMX при закрытом browser.

---

# DEV-012 — systemd, diagnostics and fully offline deployment

## Цель

Оформить DMXWB как штатно устанавливаемый daemon WB8 и доказать полностью офлайн installation bundle.

К этому gate target build path на ноутбуке уже должен быть подтверждён ранними hardware gates. DEV-012 доводит его до воспроизводимого production artifact/bundle, а не переносит compilation на WB8.

## Текущий execution order после Confirmed DEV-012B3 и независимой проверки

DEV-012A, DEV-012B и корректирующая последовательность DEV-012B1 -> DEV-012B4
уже Confirmed. Первая независимая проверка после DEV-012B3 выявила 11 дефектов,
исправленных в DEV-012B4. Вторая независимая Ultra-проверка после его regression
PASS выявила ещё 8 конкретных путей отказа. До начала offline bundle они
устраняются отдельным corrective gate DEV-012B5 без расширения DMXWB до
самостоятельной SCADA.

Фиксированный порядок:

```text
DEV-012A   production daemon and foreground acceptance — Confirmed
DEV-012B   systemd and essential operational diagnostics — Confirmed
DEV-012B1  production diagnostics contract correction — Confirmed
DEV-012B2  production / engineering instrumentation separation — Confirmed
DEV-012B3  WB8 regression after counter isolation — Confirmed
DEV-012B4  independent audit remediation — Confirmed
DEV-012B5  post-regression independent audit remediation — Confirmed
DEV-012C   offline bundle and installer
DEV-012D   offline WB8 acceptance
DEV-012E   Web and installation/maintenance instructions, gate closeout
```

### DEV-012B1 — production diagnostics contract correction

Scope:

- согласовать в `TECHNICAL_SPEC.md` разделение production operational state и engineering/test instrumentation;
- исключить cumulative test/acceptance counters из production contract;
- сохранить algorithmic revisions/generations, protocol sequence state, config revision и stable Fixture/Group/Scene ID generators;
- production diagnostics должны описывать factual current state, конфигурацию, source, ошибки и recovery state, а не lifetime telemetry;
- C++ implementation в B1 не изменяется.

PASS:

- `TECHNICAL_SPEC.md` больше не требует cumulative engineering counters в production;
- допустимый production operational state описан явно;
- algorithmic revisions/generations/IDs не ошибочно классифицированы как telemetry;
- активный шаг переходит к DEV-012B2.

### DEV-012B2 — production / engineering instrumentation separation

Scope:

- сохранить engineering counters, необходимые unit/integration/acceptance tests;
- production `dmxwb` не должен накапливать test-only counters;
- убрать test-only instrumentation из production DMX, Art-Net, MQTT, router/coordinator paths, особенно из physical DMX hot path;
- не создавать вторую реализацию DMX/MQTT/Art-Net algorithms;
- `/dmxwb/status` должен быть factual state-oriented;
- journald recovery events определяются по переходам состояния, а не historical counter deltas;
- static/build/acceptance checks обновляются под исправленный contract;
- production build должен проверяться на отсутствие запрещённых cumulative telemetry fields.

PASS:

- native Linux warnings-as-errors build PASS;
- все host tests PASS с engineering instrumentation;
- production build не накапливает test-only counters;
- production `/dmxwb/status` не содержит запрещённых cumulative telemetry fields;
- Bullseye ARM64 build и architecture/GLIBC/dependency audit PASS;
- активный шаг переходит к DEV-012B3.

### DEV-012B3 — WB8 regression after counter isolation

Scope:

- проверить corrected production `dmxwb` через systemd на реальном WB8;
- MQTT -> physical DMX, Art-Net -> physical DMX и explicit Source switching;
- restart Mosquitto -> in-process MQTT recovery без смены PID `dmxwb` и без остановки physical DMX;
- `/dmxwb/status` остаётся factual и не содержит запрещённых cumulative counters;
- journald содержит bounded lifecycle/error/recovery transition events, а не telemetry stream;
- clean stop, state flush и serial release.

PASS:

- production behavior, подтверждённый DEV-012A/DEV-012B, остаётся рабочим;
- MQTT broker recovery остаётся in-process;
- physical DMX остаётся непрерывным в recovery check;
- production status не содержит запрещённых cumulative counters;
- journald остаётся bounded и operational;
- clean shutdown/state flush/serial release PASS;
- активный шаг переходит к DEV-012B4.

### DEV-012B4 — independent audit remediation

Цель: до упаковки production bundle устранить все 11 подтверждённых замечаний
независимой проверки и доказать исправление поведением приложения. Gate имеет
высокую сложность и выполняется только следующими фиксированными подшагами:

```text
DEV-012B4.1  physical-output concurrency and Art-Net retry
DEV-012B4.2  coherent config/state persistence transaction
DEV-012B4.3  bounded valid names and monotonic stable IDs
DEV-012B4.4  persistence failure backoff and recoverable health
DEV-012B4.5  single factual operational-status owner
DEV-012B4.6  bounded Web command completion
DEV-012B4.7  focused corrective regression on host and WB8
```

Порядок фиксирован. Изменение состава, порядка или PASS-критериев оставшихся
подшагов требует явной ревизии roadmap до продолжения реализации.

#### DEV-012B4.1 — physical-output concurrency and Art-Net retry

Scope:

- обеспечить синхронизированное владение `DmxOutputPhysicalSink`, текущим
  `DmxOutput` и latest whole snapshot между main runtime и Art-Net worker;
- runtime DMX Port reconfiguration, status read и shutdown не должны пересекаться
  с publication через уничтоженный или частично заменённый output;
- при неуспешной physical publication Art-Net generation остаётся pending и
  повторяется с ограниченной частотой до успеха или появления более новой
  generation;
- после успешной доставки одна generation не публикуется повторно.

Functional tests / PASS:

- детерминированный concurrent test блокирует publication в момент port
  reconfiguration и доказывает отсутствие use-after-free, data race и потери
  последнего whole snapshot;
- transient physical publish failure -> та же Art-Net generation успешно
  доставляется повторно без нового ArtDmx;
- retry ограничен по частоте, а после успеха duplicate publication отсутствует;
- существующие DMX source/router/coordinator tests PASS.

#### DEV-012B4.2 — coherent config/state persistence transaction

Scope:

- успешное изменение Fixture-конфигурации должно оставлять на диске согласованную
  пару config/state;
- аварийное завершение или ошибка записи на любой границе транзакции после restart
  дают либо прежнюю согласованную модель, либо новую согласованную модель, но не
  смешанную пару и не silent reset к defaults;
- MQTT success result публикуется только после durable commit согласованной модели.

Functional tests / PASS:

- fault injection на каждой границе config/state commit и последующий simulated
  restart никогда не создают mismatched Fixture IDs;
- add/remove/reorder Fixture сохраняет состояние оставшихся stable IDs;
- failed transaction не меняет active in-memory model и возвращает error result;
- successful transaction после restart восстанавливает новую конфигурацию и её
  согласованное состояние.

#### DEV-012B4.3 — bounded valid names and monotonic stable IDs

Scope:

- Fixture/Group/Scene names принимаются только как valid UTF-8 длиной не более
  256 bytes во всех config, rename и Scene Create paths;
- перед каждым durable config commit canonical serialized config должен помещаться
  в `kPersistenceMaxFileBytes`;
- `next_fixture_id`, `next_group_id` и `next_scene_id` не могут уменьшаться через
  config transaction;
- удалённый stable ID нельзя назначить новому объекту; старые Scene snapshots не
  должны воздействовать на новый Fixture.

Functional tests / PASS:

- valid UTF-8 name на границе 256 bytes принимается, 257 bytes и malformed UTF-8
  отклоняются без изменения модели и файлов;
- oversized canonical config отклоняется до commit и следующий startup читает
  прежнюю конфигурацию;
- попытки уменьшить ID counters или повторно использовать удалённые
  Fixture/Group/Scene IDs отклоняются;
- Scene с историческим удалённым Fixture ID не изменяет какой-либо новый Fixture.

#### DEV-012B4.4 — persistence failure backoff and recoverable health

Scope:

- failed state save остаётся dirty, но повторяется с явным bounded backoff, а не на
  каждом runtime tick;
- configuration/persistence health отражает текущую ошибку и успешное recovery, а
  не навсегда сохраняет только startup result;
- после успешной исправляющей записи status возвращается из error/fallback без
  перезапуска процесса.

Functional tests / PASS:

- недоступное storage вызывает ограниченное число write attempts за проверяемый
  интервал и не блокирует DMX runtime step;
- после восстановления storage pending state сохраняется, dirty очищается и retry
  прекращается;
- `/dmxwb/status` показывает persistence/configuration error во время отказа и
  возвращается в factual running/ok после успешного recovery без restart;
- clean shutdown по-прежнему сообщает результат forced flush.

#### DEV-012B4.5 — single factual operational-status owner

Scope:

- retained `/dmxwb/status` публикуется только integrated operational runtime;
- Source/Config/Scene commands не подменяют factual subsystem state строками
  `controller` и не стирают текущую ошибку;
- Art-Net `output_mode=live` допустим только при selected ART-NET, active source,
  успешной physical publication и operational/open physical DMX output;
- при serial failure используется factual error/hold state до реального recovery.

Functional tests / PASS:

- Source/Config/Scene commands при существующей DMX, MQTT, Art-Net или persistence
  ошибке не создают ложное промежуточное `running` status;
- закрытый/неоткрывающийся serial port не допускает Art-Net `output_mode=live`;
- после physical recovery status переходит в `live` только по фактическому
  состоянию;
- periodic, reconnect и command publication paths сохраняют одного владельца и
  одну схему `/dmxwb/status`.

#### DEV-012B4.6 — bounded Web command completion

Scope:

- Config Set и все Web Scene operations, включая Apply, имеют request-correlated
  backend success/error result;
- Web pending state всегда завершается matched result, подтверждённым factual
  update, потерей соединения или bounded timeout;
- broker-connected/daemon-offline не оставляет Settings/Scenes заблокированными;
- потерянная QoS 0 команда не replay-ится автоматически после reconnect.

Functional tests / PASS:

- при работающем broker и отсутствующем daemon потерянные Config/Scene commands
  завершаются timeout error и UI позволяет безопасный повтор;
- Scene Apply после удаления Scene другой вкладкой получает correlated `not_found`
  и очищает pending state;
- matched success/error result очищает только соответствующий request;
- reconnect очищает pending без автоматического повторного выполнения команды;
- обычные Config и Scene операции через Web остаются работоспособны.

#### DEV-012B4.7 — focused corrective regression on host and WB8

Scope:

- выполнить только функциональную регрессию подсистем, изменённых в B4.1-B4.6;
- подтвердить production build для WB8 тем же Bullseye ARM64 toolchain;
- на WB8 проверить наиболее рискованные physical/runtime границы без повторения
  полного DEV-012B3 acceptance.

Functional tests / PASS:

- native Linux warnings-as-errors build и полный CTest suite PASS;
- static Web functional tests для timeout/correlation/reconnect PASS;
- Bullseye ARM64 build и architecture/GLIBC/dependency checks PASS;
- WB8: Art-Net traffic во время runtime DMX Port reconfiguration не вызывает crash,
  зависание или потерю последнего whole snapshot;
- WB8: config/state после service restart согласованы и восстановлены;
- WB8: serial failure/recovery даёт factual status и восстанавливает physical DMX;
- базовые MQTT -> DMX, Art-Net -> DMX и explicit Source switching остаются рабочими;
- после PASS активный шаг переходит к DEV-012B5.1.

### DEV-012B5 — post-regression independent audit remediation

Цель: до offline packaging устранить все 8 подтверждённых второй независимой
проверкой путей отказа. Gate имеет высокую сложность и выполняется только
следующими фиксированными подшагами:

```text
DEV-012B5.1  Art-Net accepted-traffic liveness and sequence recovery
DEV-012B5.2  persistence fallback and file-identity safety
DEV-012B5.3  factual daemon availability gate for Web commands
DEV-012B5.4  durable retained MQTT cleanup
DEV-012B5.5  Config uncertain-outcome reconciliation
DEV-012B5.6  idempotent Scene Create retry
DEV-012B5.7  strict Web numeric validation
DEV-012B5.8  focused corrective regression on host and WB8
```

Порядок фиксирован. Изменение состава, порядка или PASS-критериев оставшихся
подшагов требует явной ревизии roadmap до продолжения реализации.

#### DEV-012B5.1 — Art-Net accepted-traffic liveness and sequence recovery

Scope:

- Art-Net source activity и трёхсекундный LOST timeout обновляются только
  принятым ArtDmx, прошедшим source и Sequence validation;
- duplicate, out-of-order и иные отклонённые пакеты не могут удерживать source в
  ACTIVE и не могут бесконечно удерживать старый physical snapshot;
- после LOST sequence baseline сбрасывается, поэтому перезапущенный контроллер с
  меньшим Sequence восстанавливается без ожидания полного обхода 1..255;
- штатная последовательность, wrap-around, Sequence=0, ArtSync и source conflict
  сохраняют существующую семантику.

Functional tests / PASS:

- принятый Sequence=128, затем отклоняемые 1..N или бесконечный повтор одного
  номера -> source становится LOST не позднее нормативного timeout;
- первый допустимый пакет после LOST принимается как новая baseline и целиком
  заменяет старый snapshot;
- отклонённые пакеты не изменяют last accepted activity timestamp;
- существующие Art-Net sequence/sync/conflict/recovery tests PASS.

#### DEV-012B5.2 — persistence fallback and file-identity safety

Scope:

- invalid/unreadable config может запустить только безопасный fallback runtime,
  но fallback configuration не имеет права reconcile, очищать или перезаписывать
  ранее существующий state;
- после восстановления корректного config сохранённые Fixture values должны быть
  доступны без silent reset;
- production startup до создания runtime обязан отклонять одинаковые или
  фактически указывающие на один файл `--config` и `--state` пути;
- отказ проверки путей происходит до любой записи config/state.

Functional tests / PASS:

- corrupt config + valid state, runtime ticks, forced flush и shutdown оставляют
  state byte-for-byte неизменным;
- после возврата корректного config прежние Fixture values восстанавливаются;
- одинаковый literal path, equivalent normalized path, symlink и hard-link alias
  config/state отклоняются до запуска runtime и не изменяют файл;
- разные корректные пути сохраняют существующие load/save/recovery semantics.

#### DEV-012B5.3 — factual daemon availability gate for Web commands

Scope:

- browser MQTT connection к broker больше не считается достаточным признаком
  доступности DMXWB backend;
- Web подписывается на штатный retained/LWT Status control и объединяет его с
  `/dmxwb/status` для factual daemon availability;
- команды блокируются при daemon `off`/отсутствии, pending очищается как
  неопределённый результат, а после фактического startup управление включается;
- subsystem `error` живого daemon не блокирует команды, необходимые для recovery.

Functional tests / PASS:

- broker connected + daemon stopped -> все command controls disabled и MQTT
  command publication отсутствует;
- crash без graceful status -> LWT `off` блокирует команды и очищает pending;
- restart включает команды только после factual online status;
- живой daemon с recoverable DMX/persistence error остаётся управляемым.

#### DEV-012B5.4 — durable retained MQTT cleanup

Scope:

- удаление Fixture/Group/Scene создаёт durable cleanup intent до потери сведений о
  прежних stable IDs;
- retained tombstones доставляются с подтверждаемой MQTT delivery и повторяются
  после reconnect/restart до подтверждения;
- повторная tombstone publication идемпотентна и не удаляет retained topics
  существующих entities;
- normal full republish и cleanup используют один согласованный набор entity IDs.

Functional tests / PASS:

- disconnect между config commit и tombstone delivery -> reconnect удаляет все
  старые retained entity topics;
- process kill на границах durable intent / publish / delivery confirmation ->
  restart завершает cleanup без потери текущих topics;
- повторная доставка tombstone безопасна;
- обычные add/remove/reconnect и standard WB hidden-control contracts PASS.

#### DEV-012B5.5 — Config uncertain-outcome reconciliation

Scope:

- timeout/disconnect после Config Set сохраняет данные исходного request и
  proposal для последующей factual reconciliation, а не выдаёт безусловный safe
  retry со старой revision;
- retained config, совпавший с proposal кроме серверной revision, подтверждает уже
  выполненную операцию и очищает dirty draft даже после reconnect;
- неизменившаяся factual revision делает proposal повторяемым, а отличающаяся
  чужая конфигурация переводит draft в явный stale/conflict state;
- Web никогда автоматически не replay-ит Config Set.

Functional tests / PASS:

- применённый Config + потерянный result + reconnect -> factual match завершает
  операцию без повторной публикации и без revision conflict;
- неприменённая потерянная команда остаётся доступной для явного retry с актуальной
  допустимой base revision;
- concurrent отличающийся Config показывает conflict и не отправляет заведомо
  устаревший expected_revision;
- нормальные success/error/timeout paths остаются request-correlated.

#### DEV-012B5.6 — idempotent Scene Create retry

Scope:

- Scene Create получает bounded durable idempotency record по `request_id`;
- повтор того же request с тем же payload возвращает первоначальный Scene ID и не
  создаёт новую Scene, включая reconnect и process restart;
- повтор request ID с другим payload отклоняется как idempotency conflict;
- Web при неопределённом результате повторяет только тот же request ID и не
  обещает безопасный новый Create.

Functional tests / PASS:

- потеря результата после committed Create и повтор того же request -> одна Scene,
  один stable ID и тот же correlated outcome;
- тот же сценарий после restart сохраняет идемпотентность;
- одинаковый request ID с другим Name отклоняется без изменения конфигурации;
- normal Create, ID monotonicity и bounded durable-record retention PASS.

#### DEV-012B5.7 — strict Web numeric validation

Scope:

- каждое числовое structural field имеет явное valid/invalid состояние и проверку
  как при вводе, так и непосредственно перед Config Set publication;
- текст в input и фактически отправляемая proposal не могут молча расходиться;
- при invalid field Apply disabled, показывается конкретная ошибка и MQTT command
  не публикуется;
- исправление значения обновляет draft ровно введённым допустимым значением.

Functional tests / PASS:

- Fixture Count=76 и другие out-of-range/non-numeric values остаются видимыми как
  invalid, блокируют Apply и не публикуют старое значение;
- minimum/maximum boundary values каждого numeric field принимаются;
- после исправления payload содержит именно показанное значение;
- dirty/stale/reset и normal Config Apply semantics остаются корректны.

#### DEV-012B5.8 — focused corrective regression on host and WB8

Scope:

- выполнить функциональную регрессию только подсистем, изменённых в B5.1-B5.7;
- подтвердить native warnings-as-errors, полный CTest и Bullseye ARM64 production
  build без повторения уже закрытых широких acceptance;
- на WB8 проверить наиболее рискованные recovery/durability границы.

Functional tests / PASS:

- native Linux warnings-as-errors build, полный CTest и затронутые Web functional
  tests PASS;
- Bullseye ARM64 architecture/GLIBC/dependency checks PASS;
- WB8 Art-Net controller Sequence restart не удерживает старый кадр дольше timeout
  и восстанавливает новый whole snapshot;
- corrupt config recovery не изменяет сохранённый state;
- daemon stop/crash/restart корректно блокирует и возвращает Web commands;
- retained cleanup и Scene Create idempotency переживают disconnect/restart;
- после PASS активный шаг переходит к DEV-012C.

### DEV-012E — Web and installation/maintenance instructions

DEV-012E выполняется только после фактического PASS offline bundle и WB8 acceptance
в DEV-012C/DEV-012D, чтобы документация описывала проверенное приложение и реальные
команды, а не предварительные предположения.

Scope:

- подготовить `docs/WEB_USER_GUIDE.md` с пошаговым описанием Web-интерфейса:
  подключение, состояния связи/daemon, Source, Fixture, Group, Scene, настройка
  конфигурации, Apply/Reset, validation/revision conflict и восстановление связи;
- подготовить `docs/INSTALL_UPDATE_REMOVE_GUIDE.md` с отдельными пошаговыми
  процедурами первоначальной offline-установки, обновления, удаления приложения и,
  если поддерживается, полного удаления пользовательских config/state;
- явно разделить безопасное удаление приложения с сохранением пользовательских
  данных и необратимый purge; purge не должен подразумеваться обычной командой
  удаления;
- после каждой процедуры дать отдельные команды проверки с ожидаемым результатом;
- в конце эксплуатационной инструкции дать единый список всех поддерживаемых команд
  DMXWB с кратким описанием назначения, требуемых прав и ожидаемого результата;
- не включать непроверенные команды, placeholder-пути и online-зависимости;
- добавить ссылки на обе инструкции в `README.md`.

PASS:

- Web-инструкция покрывает все доступные пользователю функции актуального UI;
- install/update/remove описаны пошагово и соответствуют принятому offline bundle;
- команды проверки отделены от команд, изменяющих систему;
- обычное обновление и удаление не уничтожают config/state без явного действия;
- заключительный справочник содержит все поддерживаемые команды и их описание;
- документация сверена с результатами DEV-012C/DEV-012D, после чего DEV-012 получает
  PASS и активный gate переходит к DEV-013.

## Реализовать

- `deploy/dmxwb.service`;
- `Type=simple`;
- `Restart=on-failure`;
- `RestartSec=2s`;
- `deploy/install_wirenboard.sh`;
- поддерживаемая offline-процедура обновления и удаления;
- production binary, собранный на ноутбуке;
- static web;
- systemd unit;
- installer;
- required local runtime files;
- `/etc/dmxwb`, `/var/lib/dmxwb`, `/var/www/dmxwb`;
- permissions;
- journald logging;
- graceful SIGTERM;
- MQTT LWT;
- startup/reconnect ordering;
- diagnostics;
- hidden Fixture/Group/Scene metadata;
- standard WB UI: только Status/Source.

Installer не выполняет:

```text
apt update
online apt install
git clone
curl/wget
npm
compiler/CMake install
source compilation on WB8
Docker operations
```

## Tests

- production build reproduced on laptop/local Linux;
- fresh install при физически недоступном внешнем интернете;
- no network downloads;
- no compiler/CMake/Node/Docker requirement on WB8;
- start/stop/restart;
- crash -> systemd recovery;
- MQTT broker restart;
- serial recovery;
- WB8 reboot;
- config/state restore;
- web after reboot;
- local Mosquitto;
- physical DMX;
- Art-Net;
- standard WB UI not polluted.
- update/remove/reinstall workflow и сохранение пользовательских config/state;
- команды из пользовательской и эксплуатационной инструкций соответствуют реально
  принятому поведению.

Для tested installation зафиксировать модель WB8, WB software version и build artifact/toolchain identity.

## PASS

Recoverable subsystem errors исправляются приложением, systemd restart нужен только
при реальном process failure, чистая установка/reboot/basic operation проходят без
внешнего интернета и без compilation на WB8, а проверенные Web и lifecycle-инструкции
соответствуют фактическому приложению.

---

# DEV-013 — full integration and final acceptance — Confirmed

## Цель

Доказать выполнение полного `TECHNICAL_SPEC.md` как единой системы на серии WB8.

## Перед началом

Requirements traceability review:

- каждый MUST связан с кодом;
- критический алгоритм связан с test;
- нет diagnostic shortcuts в production path;
- target build и deployment воспроизводимы на ноутбуке без Docker.

## Full functional acceptance

### DMX

- default `/dev/ttyRS485-1`;
- dynamic slot_count;
- Start Address !=1;
- 10/30/44 Hz when feasible;
- continuous output;
- no visible flicker.

### Fixture

R/G/B, Color, Temperature, Brightness, Power restore, Reset, MQTT state.

### Groups

Multiple memberships, controls, Power, reset.

### Scenes

Create/apply/overwrite/rename/delete/atomic visual apply.

### MQTT

Broker restart/reconnect, retained state, retained command rejection, standard WB UI.

### Art-Net

Raw mapping, short/long packets, Source switching, loss/recovery, IP change, conflict, Hold Last.

### Web

Management functions, reconnect, revision conflict, no external runtime dependencies.

### Persistence

Service restart, WB8 reboot, corruption tests, interrupted atomic update where safely testable.

### Build/offline installation

На ноутбуке:

- clean/reproducible target build;
- production bundle creation;
- no Docker.

На WB8 с отключённым внешним интернетом:

- install only from local bundle;
- no online packages/downloads;
- no source compilation;
- reboot;
- systemd service;
- local web/MQTT;
- physical DMX;
- Art-Net.

### Platform record

Для каждой acceptance configuration записать:

```text
WB8 model
WB software/OS version
DMX port
binary/build identity
toolchain/sysroot identity
```

## FINAL PASS

- all automated tests PASS;
- hardware/network acceptance PASS;
- target build PASS;
- offline installation PASS;
- no manual restart after recoverable failures;
- physical DMX и Art-Net выполняют главную функцию на реальном WB8;
- документация соответствует приложению;
- PROJECT_STATE содержит итоговую tested configuration.

Status: **Confirmed / FINAL PASS** по пользовательским результатам DEV-013A,
DEV-013B, DEV-013C и DEV-013D. Итоговая платформа, artifact identity и ссылки на
четыре acceptance-отчёта зафиксированы в `docs/PROJECT_STATE.md`.

---

# DEV-014 — corrective entity-management completion

## Причина

После DEV-013 пользовательская проверка естественного сценария с пустой
конфигурации выявила несоответствие продуктовой модели: светильники создавались
изменением числового количества, удалить произвольный светильник было нельзя, а
Fixture/Group/Scene были скрыты из стандартного WB HomeUI. Технический DEV-013 PASS
сохраняется как доказательство подсистем, но пользовательский продукт требует
коррекции и нового финального пакета.

## DEV-014A — explicit Fixture CRUD in dedicated DMXWB Web

- отдельный список светильников в структурных настройках;
- явные действия `Добавить светильник` и `Удалить`;
- stable Fixture ID не переиспользуются;
- удаление произвольного Fixture очищает его membership во всех Group;
- Scene snapshots сохраняют исторические stable IDs;
- количество Fixture вычисляется по списку, а не вводится пользователем;
- при пустой конфигурации список Fixture пуст.

PASS: из пустого draft можно сначала создать светильники, затем создать Group и
выбрать участников; любой выбранный Fixture удаляется до Apply; отображаются его
derived DMX addresses; полный Config Set остаётся атомарным и revision-safe.

## DEV-014B — standard WB HomeUI visibility

- созданные Fixture, Group и Scene публикуются как видимые WB MQTT devices;
- стандартный WB HomeUI предоставляет их live controls;
- создание/удаление структуры выполняется только в `/dmxwb/`;
- системные Status/Source и явная граница WB MQTT / ART-NET сохраняются.

PASS: после создания в `/dmxwb/` Fixture/Group/Scene появляются в стандартном WB
HomeUI и управляют тем же фактическим MQTT-состоянием.

## DEV-014C — clean-state acceptance and replacement final package

- чистая конфигурация начинается с `0` Fixtures, Groups и Scenes;
- реальный путь: Add Fixture -> Add Fixture -> Add Group -> membership -> Scene;
- удаление выбранного Fixture и очистка membership;
- сохранение после service restart и полного reboot;
- новый host/ARM64 build и полностью офлайн-пакет;
- инструкции и итоговая artifact identity соответствуют исправленному приложению.

PASS: исправленный workflow подтверждён на реальном WB8, новый offline installer
получает финальный PASS и заменяет пакет DEV-013A.

---

## 3. Документация по этапам

После значимого шага обновляется документация, которую реально затронуло изменение.

Минимально:

- `PROJECT_STATE.md` — когда изменилось фактическое состояние, текущий gate, tested environment или ближайший шаг;
- `README.md` — когда изменились пользовательские build/install/run commands или точка входа;
- `TECHNICAL_SPEC.md` — только при согласованном изменении требований;
- `ROADMAP.md` — при изменении порядка/scope/PASS criteria будущих gates;
- `AGENTS.md` — при изменении процесса взаимодействия.

Не создавать конкурирующие архитектурные документы без необходимости.

---

## 4. Принцип остановки

Если текущий gate выявляет фундаментальную проблему, следующий engineering gate не начинается.

```text
DEV-003 target build / physical DMX FAIL
-> не начинать Fixture/MQTT/Art-Net

DEV-004 unstable continuous DMX
-> не начинать application layers

DEV-010 Art-Net recovery FAIL
-> не считать network mode готовым
```

Документационные/process шаги внутри текущего gate разрешены и не считаются перескакиванием через gate.

---

## 5. Как продолжать после каждого SHA

После получения нового полного SHA пользователя:

1. считать предыдущий шаг внесённым пользователем в репозиторий;
2. заново прочитать актуальный `AGENTS.md` и `PROJECT_STATE.md`;
3. проверить актуальное состояние GitHub, а не полагаться только на branch name или старый локальный snapshot;
4. определить следующий шаг/текущий engineering gate;
5. скачать необходимые файлы из актуального репозитория;
6. выполнить только scope выбранного шага;
7. обновить относящуюся документацию;
8. подготовить root-relative ZIP с финальными файлами;
9. передать handoff по `AGENTS.md`;
10. при FAIL продолжить тот же шаг/gate;
11. при следующем commit SHA снова начать с актуального репозитория.

Нельзя перескакивать через hardware gates ради UI/MQTT/Art-Net. Конечный критерий — полностью рабочее управление реальным DMX-освещением и надёжное Art-Net управление на контроллерах серии WB8.
