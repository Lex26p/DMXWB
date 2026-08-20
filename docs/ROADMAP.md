# DMXWB DEVELOPMENT ROADMAP

**Статус:** рабочая дорожная карта реализации утверждённого `TECHNICAL_SPEC.md`  
**База актуализации дорожной карты:** `e6b563da756023a3e87afe38fb25e59a98d7d21e`  
**Дата актуализации:** 2026-08-20  
**Цель:** последовательно реализовать полное приложение DMXWB с проверяемым PASS/FAIL после каждого значимого engineering gate.

Дорожная карта является рабочей инструкцией по порядку разработки. Источник истины — актуальное состояние репозитория. Перед каждым шагом необходимо прочитать `AGENTS.md`, `PROJECT_STATE.md`, относящиеся к задаче части `TECHNICAL_SPEC.md` и настоящий roadmap.

Пользователь самостоятельно вносит подготовленные изменения в репозиторий. Новый полный SHA после пользовательского commit/push подтверждает завершение конкретного шага и разрешает переход к следующему шагу, если он предусмотрен. Engineering gate получает PASS только по своим фактическим критериям — один только documentation commit или наличие кода в `master` не заменяет hardware/integration test.

Текущий продуктовый приоритет: физический DMX является ядром системы; Art-Net — полноценный внешний источник того же DMX-выхода; MQTT/Web — средства локального управления и интеграции с Wiren Board; production installation bundle устанавливается полностью офлайн.

Целевая платформа — **контроллеры серии Wiren Board 8 (WB8)**. DMXWB является расширением возможностей Wiren Board, а не заменой контроллера или самостоятельной SCADA.

Development/build environment:

```text
Windows + Visual Studio 2026 -> host development/tests
Local Linux on laptop        -> Linux/target build and production artifacts
WB8                          -> runtime/hardware/integration acceptance
Docker                       -> не используется
```

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
- host build/tests выполняются на ноутбуке пользователя;
- Windows / Visual Studio 2026 используется для host development/tests;
- локальный Linux используется для Linux-specific и target build;
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
DEV-013  Full integration, offline install and 24h acceptance
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
| `DEV-013` | Полная функциональная и 24h acceptance | Реальная WB8-система целиком |

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

Превратить доказанный transport в независимый непрерывный DMX output engine.

## Реализовать

Отдельный `DmxOutput` worker:

- единственный владелец serial fd;
- continuous BREAK + frame loop;
- frame-start scheduling по абсолютному времени;
- configurable refresh 10..44 Hz, default 30 Hz;
- validation физически достижимого refresh;
- новый snapshot только между кадрами;
- runtime refresh change без закрытия serial, если корректно;
- serial error detection;
- close/reopen/retry;
- recovery с актуального snapshot;
- diagnostics/counters.

## Tests

Host/unit:

- scheduler arithmetic;
- impossible refresh rejected;
- snapshot switch only at frame boundary;
- no mixed generation frame;
- retry-state transitions.

WB8 hardware:

- 10 Hz;
- 30 Hz;
- 44 Hz, когда длина кадра допускает;
- runtime refresh change;
- continuous fixed-pattern smoke;
- recoverable serial error/recovery, если безопасно воспроизвести.

## PASS

- stable continuous DMX;
- no visible flicker;
- refresh соответствует допустимому диапазону;
- recoverable serial error не требует process restart.

---

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
- validation <=512;
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

Реализовать и unit-test Art-Net parser/state machine без реального UDP runtime.

## Реализовать

- Art-Net 4 constants;
- safe packet parsing;
- ArtDmx/ArtPoll/ArtPollReply;
- signature/opcode/protocol validation;
- Port-Address/Universe;
- even Length 2..512;
- persistent artnet_state[512];
- short packet Hold Last;
- long packet truncation;
- new channels zero-init when slot_count grows;
- Sequence ordering/rollover/reset;
- WAITING/ACTIVE/LOST/CONFLICT;
- active-source IP lock;
- timeout release;
- no HTP/LTP merge.

## Tests

Deterministic packet fixtures для header/opcode/version/length/universe, short/long packets, rollover, source reboot, conflict и lock release.

## PASS

Art-Net core не требует socket/real time для tests.

---

# DEV-010 — Art-Net runtime, reliability and Source switching

## Цель

Подключить Art-Net core к UDP 6454 и доказать автоматическое восстановление реальной связи.

## Реализовать

Art-Net worker:

- IPv4 UDP 6454;
- bind/rebind;
- Poll/PollReply/Dmx;
- 3 s LOST timeout;
- socket recovery;
- diagnostics;
- active/conflict source IP;
- last packet age;
- Hold Last.

Source selector:

- MQTT / ART-NET;
- switch only between frames;
- both inputs live;
- no first ArtDmx -> preserve current physical output until snapshot;
- LOST -> Hold Last, Source stays ART-NET;
- return -> automatic without restart.

## Software test source

На ноутбуке выбрать Art-Net приложение/контроллер и зафиксировать название/версию в PROJECT_STATE перед network acceptance.

## Reliability tests

При активном Art-Net:

- Ethernet disconnect ~0.5/5/30 s;
- restart/power cycle source;
- IP change;
- WB8 network down/up;
- repeated reconnect;
- second source conflict if reproducible.

## PASS

После временного Art-Net failure оператор не выполняет ручных действий для восстановления DMXWB.

---

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

## Реализовать

- `deploy/dmxwb.service`;
- `Type=simple`;
- `Restart=on-failure`;
- `RestartSec=2s`;
- `deploy/install_wirenboard.sh`;
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

Для tested installation зафиксировать модель WB8, WB software version и build artifact/toolchain identity.

## PASS

Recoverable subsystem errors исправляются приложением, systemd restart нужен только при реальном process failure, а чистая установка/reboot/basic operation проходят без внешнего интернета и без compilation на WB8.

---

# DEV-013 — full integration, offline installation and final acceptance

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

## 24-hour test

Не менее 24 часов работы с Fixture/Group/Scene changes, source switching, Art-Net, network disconnect/reconnect, MQTT broker restart и uptime/restart monitoring.

## FINAL PASS

- all automated tests PASS;
- hardware/network acceptance PASS;
- target build PASS;
- offline installation PASS;
- 24-hour test PASS;
- no manual restart after recoverable failures;
- physical DMX и Art-Net выполняют главную функцию на реальном WB8;
- документация соответствует приложению;
- PROJECT_STATE содержит итоговую tested configuration.

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
