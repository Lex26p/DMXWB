# DMXWB DEVELOPMENT ROADMAP

**Статус:** рабочая дорожная карта реализации утверждённого `TECHNICAL_SPEC.md`  
**База актуализации дорожной карты:** `e111c1b5fd6df1b3df3a9c8ff2a68d8c1a230616`  
**Цель:** последовательно реализовать полное приложение DMXWB с проверяемым PASS/FAIL после каждого значимого этапа.

Дорожная карта является рабочей инструкцией для последующих моделей. Перед началом любого шага модель обязана прочитать `AGENTS.md`, затем `PROJECT_STATE.md`, определить текущий gate и выполнять **только его**. После успешного handoff пользователь присылает новый полный SHA; только после этого разрешён переход к следующему gate.

Текущий продуктовый приоритет: физический DMX является ядром системы; Art-Net — полноценный внешний источник того же DMX-выхода; MQTT/Web — средства локального управления и интеграции Wiren Board; production installation bundle обязан устанавливаться полностью офлайн.

---

## 1. Правила дорожной карты

Дорожная карта описывает порядок разработки, но не заменяет техническое задание.

При конфликте документов приоритет:

1. `docs/TECHNICAL_SPEC.md` — что должно быть реализовано в конечном приложении.
2. `docs/ROADMAP.md` — в каком порядке это реализуется и проверяется.
3. `docs/PROJECT_STATE.md` — где именно находится текущая работа.

Каждый этап:

- имеет одну основную инженерную цель;
- строится только от полного SHA предыдущего PASS-коммита;
- включает код, тесты и относящуюся к этапу документацию;
- не считается завершённым только потому, что код компилируется;
- получает PASS только после предусмотренных автоматических и/или hardware tests;
- при FAIL остаётся текущим этапом до устранения причины;
- не расширяется побочными задачами, не нужными для его gate.

Разработка идёт к одному полному приложению. Этапы не являются отдельными продуктовыми версиями.

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
| `DEV-001` | C++20/CMake foundation и test harness | Windows/dev host |
| `DEV-002` | Immutable DMX snapshots, channel/slot model, frame core | Host unit tests |
| `DEV-003` | Минимальный физический DMX transport через RS-485 | Реальный Wiren Board + светильник |
| `DEV-004` | Непрерывный DMX engine, timing, refresh, serial recovery | Host + Wiren Board |
| `DEV-005` | RGBW Fixture model, addressing, stable IDs | Host + DMX smoke |
| `DEV-006` | Config/state persistence и атомарные транзакции | Host/integration tests |
| `DEV-007` | WB MQTT system + Fixture contract | Host/WB MQTT + DMX |
| `DEV-008` | Groups и Scenes | Host + несколько DMX адресов |
| `DEV-009` | Art-Net protocol parser/state machine | Host unit tests |
| `DEV-010` | Art-Net UDP runtime, recovery, source switching | WB + внешний Art-Net источник |
| `DEV-011` | Статический MQTT-only Web UI | Browser + локальный MQTT |
| `DEV-012` | systemd, diagnostics и полностью офлайн deployment bundle | Чистая offline-установка на WB |
| `DEV-013` | Полная функциональная и 24h acceptance | Реальная система целиком |

Эта таблица задаёт границы. Подробный scope каждого gate ниже является обязательным. Задачу будущего gate не переносить в текущий без отдельной причины и изменения roadmap.

Критический путь проекта:

```text
C++ foundation
    -> physical DMX
    -> stable continuous DMX
    -> application logic
    -> MQTT
    -> Art-Net reliability
    -> Web
    -> final acceptance
```

Физический DMX проверяется раньше остальных крупных подсистем, потому что именно он является основой всего приложения.

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
- правила warning/error для production build;
- deterministic unit-test runner;
- базовые типы ошибок/результатов, только если они реально нужны текущему коду.

До добавления новой сторонней runtime-зависимости подтвердить её доступность/пригодность для целевого WB8 ARM64.

## Не включать

- serial;
- MQTT;
- Art-Net;
- persistence;
- systemd;
- web;
- полноценную Fixture/Group/Scene модель.

## Tests

Минимум:

- clean CMake configure;
- clean build;
- unit-test executable запускается;
- intentionally simple test PASS;
- production executable запускается без обращения к hardware и корректно завершается в тестовом режиме/с минимальным CLI, если такой режим нужен.

## PASS

- проект собирается с нуля;
- unit tests = PASS;
- нет hardware side effects;
- структура проекта соответствует `TECHNICAL_SPEC.md`;
- `README.md` и `PROJECT_STATE.md` обновлены фактическими командами сборки/тестов.

---

# DEV-002 — DMX core types and deterministic frame model

## Цель

Создать hardware-independent ядро данных, которое физический DMX thread позже сможет получать целым immutable snapshot.

## Реализовать

- `DmxSnapshot`;
- максимум 512 каналов;
- `slot_count`;
- generation/revision snapshot;
- безопасную индексацию DMX channel 1..512;
- расчёт `slot_count` по последнему используемому адресу;
- модель физического payload:
  - Start Code отдельно от channels;
  - channels 1..N;
- double-buffer/immutable snapshot publication primitive либо эквивалентную безопасную схему;
- clock/scheduling helper interface, не привязанный пока к serial.

## Не включать

- реальный `termios`;
- BREAK;
- Fixture color algorithms;
- MQTT;
- Art-Net.

## Tests

- channel 1 и channel 512;
- invalid channel rejection;
- `slot_count = 40`;
- `slot_count = 60` при Start Address 21 / 10 RGBW fixtures — только на уровне расчётного helper;
- snapshot не может наблюдаться частично обновлённым;
- Start Code не создаёт off-by-one относительно channel 1;
- копирование/публикация snapshot сохраняет generation и data целостно.

## PASS

Все core tests deterministic и не требуют WB.

---

# DEV-003 — physical DMX transport proof

## Цель

Доказать на реальном Wiren Board 8.5.1, что C++ transport способен физически выдавать рабочий DMX512 через встроенный `/dev/ttyRS485-1`.

Это первый обязательный hardware gate проекта.

## Реализовать

Минимальный `DmxTransport`:

- открыть выбранный serial;
- базовый default `/dev/ttyRS485-1`;
- data mode `250000 8N2`;
- программный BREAK по проверенному WB-подходу:
  - 38400;
  - write `0x00`;
  - flush;
  - возврат 250000 8N2;
- отправить Start Code `0x00`;
- отправить небольшой фиксированный набор channels;
- корректно закрыть порт.

Для gate разрешён отдельный diagnostic CLI/test mode, если он не смешивается с production runtime logic.

## Hardware test

На реальном RGBW fixture с известным стартовым адресом:

- все каналы 0;
- R only;
- G only;
- B only;
- W only;
- все четыре = 255.

При наличии безопасного измерительного оборудования проверить реальный bit timing/BREAK. Подключение earth-grounded oscilloscope к A/B выполнять только после подтверждения безопасной схемы измерения.

## STOP CONDITION

Если C++-реализация проверенного WB BREAK/serial метода не даёт стабильный физический DMX:

- не переходить к Fixture/MQTT/Art-Net;
- не маскировать проблему retries/UI;
- оставаться на DEV-003;
- измерить и исправить именно transport.

Возврат к custom kernel/WBEC не является автоматическим решением и требует отдельного изменения ТЗ.

## PASS

- реальный fixture устойчиво реагирует на все фиксированные тестовые patterns;
- процесс не требует patch kernel/WBEC;
- serial после теста корректно освобождается;
- команды и фактический hardware result документированы.

---

# DEV-004 — continuous DMX engine, timing and serial recovery

## Цель

Превратить доказанный transport в независимый непрерывный DMX output engine.

## Реализовать

Отдельный `DmxOutput` worker:

- единственный владелец serial fd;
- непрерывный BREAK + frame loop;
- frame start scheduling по абсолютному времени;
- configurable refresh:
  - min 10 Hz;
  - max protocol limit 44 Hz;
  - default 30 Hz;
- проверку физически достижимого refresh для текущего `slot_count`;
- применение нового snapshot только между кадрами;
- runtime изменение refresh без закрытия serial, если это технически корректно;
- serial error detection;
- close/reopen/retry;
- автоматическое восстановление с актуального snapshot;
- counters/diagnostics:
  - frames sent;
  - configured refresh;
  - measured/actual refresh;
  - last transport error.

## Tests

Host/unit:

- scheduler arithmetic;
- невозможный refresh отклоняется;
- snapshot switch только на frame boundary;
- no mixed generation frame;
- retry-state transitions.

WB hardware:

- 10 Hz;
- 30 Hz;
- 44 Hz, когда длина кадра допускает;
- изменение частоты на лету;
- длительный непрерывный fixed-pattern smoke test;
- временная ошибка/освобождение порта и автоматический recovery, если её можно безопасно воспроизвести.

## PASS

- стабильный непрерывный DMX;
- нет заметного flicker на hardware smoke test;
- runtime refresh работает в допустимом диапазоне;
- recoverable serial error не требует restart процесса.

---

# DEV-005 — Fixture RGBW model and addressing

## Цель

Реализовать утверждённую модель RGBW-светильника независимо от MQTT/Web.

## Реализовать

`Fixture`:

- stable ID;
- Name;
- requested Power;
- R/G/B/W;
- Brightness;
- Temperature;
- расчёт actual RGBW;
- factual Power;
- RGB/Color rule `W=0`;
- Temperature rule:
  - RGB=255;
  - W=temperature 0..255;
- Brightness scaling всех четырёх каналов;
- Power OFF сохраняет внутреннее состояние;
- Power ON восстанавливает;
- Reset = ON, Brightness 100, RGBW 255.

Fixture collection/config:

- `Fixture Count`;
- `Start Address`;
- 4 channels per Fixture;
- sequential addressing;
- validation <=512;
- `Fixture Count=0`;
- stable monotonic fixture IDs;
- ID не переиспользуются;
- изменение Start Address не меняет logical state;
- построение целого `mqtt DmxSnapshot`.

## Tests

Все алгоритмы из `TECHNICAL_SPEC.md`, включая:

- individual R/G/B takeover;
- Color;
- Temperature 0/50/100;
- Brightness;
- Power restore;
- factual Power при Brightness 0;
- Reset;
- address 1;
- non-1 Start Address;
- max boundary 512;
- count decrease/increase без ID reuse;
- atomic snapshot rebuild.

## Hardware smoke

Минимальный test fixture через уже доказанный `DmxOutput`:

- R/G/B;
- Temperature;
- Brightness;
- Power OFF/ON restore;
- Reset.

## PASS

Модель и физический output совпадают с ТЗ.

---

# DEV-006 — configuration and persistence

## Цель

Добавить каноническую конфигурацию и runtime state без влияния файлового I/O на DMX timing.

## Реализовать

- `/etc/dmxwb/config.json`;
- `/var/lib/dmxwb/state.json`;
- version;
- config revision;
- monotonic counters:
  - fixture;
  - group;
  - scene;
- parse/serialize;
- полную валидацию до применения;
- atomic `tmp + fsync + rename`;
- dirty state;
- 2 s debounce;
- max 10 s dirty interval;
- forced dirty save on graceful shutdown;
- safe defaults;
- поведение при corrupt state;
- поведение при corrupt config;
- config transaction:
  - validate;
  - save;
  - publish/apply atomically.

## Не включать

MQTT transport ещё не нужен: config API тестируется напрямую через C++/test helpers.

## Tests

- round trip;
- invalid schema/version;
- invalid fixture range;
- missing Group member;
- Scene with missing Fixture;
- revision mismatch;
- atomic replace failure simulation;
- corrupt state fallback;
- corrupt config fallback;
- dirty debounce logic;
- stable IDs survive restart.

## PASS

- persistence не вызывается из DMX output thread;
- корректный restart восстанавливает state;
- повреждённый новый config не заменяет рабочий.

---

# DEV-007 — MQTT system and Fixture integration

## Цель

Подключить libmosquitto и реализовать утверждённый MQTT contract для системного устройства и Fixtures.

## Реализовать

MQTT lifecycle:

- localhost broker;
- reconnect;
- subscriptions;
- non-retained commands;
- retained command rejection;
- metadata;
- retained factual state;
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

Web/internal retained snapshots:

```text
/dmxwb/config
/dmxwb/state
/dmxwb/status
```

Config transaction:

```text
/dmxwb/config/set
/dmxwb/config/result
```

MQTT callback только создаёт Commands и помещает их в Controller queue.

## Tests

- `/on -> Controller -> snapshot -> factual state`;
- `Color R;G;B`;
- factual RGB after Brightness;
- factual RGB zero after OFF;
- saved state retained in `/dmxwb/state`;
- retained `/on` ignored;
- reconnect full resync;
- broker down/up without process restart;
- Source retained/restored from disk, not broker.

## Hardware integration

При WB MQTT управлять реальным fixture через MQTT CLI/client и подтвердить физический результат.

## PASS

MQTT loss/recovery не останавливает DMX loop.

---

# DEV-008 — Groups and Scenes

## Цель

Реализовать всю логическую групповую работу и snapshots сцен поверх уже работающих Fixture/MQTT/persistence.

## Реализовать

Groups:

- stable monotonic ID;
- Name;
- members by Fixture ID;
- Fixture может быть в нескольких groups;
- Power;
- R/G/B;
- Color;
- Brightness;
- Temperature;
- Reset;
- factual Group Power = OR(member factual Power);
- group setting state = последняя команда через Group;
- удаление Fixture очищает memberships;
- пустая Group допустима.

Scenes:

- stable monotonic ID;
- Name;
- create from current state;
- overwrite;
- apply;
- delete;
- snapshot:
  - Fixture ID;
  - R/G/B/W;
  - Brightness;
  - requested Power;
- missing Fixture ignored;
- newly created Fixture untouched;
- Apply не переключает Source;
- один atomic MQTT DMX snapshot при Apply.

MQTT devices для Group/Scene согласно ТЗ.

## Tests

- один Fixture в нескольких Groups;
- last command wins;
- group Power restore индивидуальных states;
- group reset;
- empty group;
- factual group Power;
- scene create/apply/overwrite/delete;
- scene after Fixture deletion/addition;
- atomic scene snapshot;
- retained topic cleanup after delete.

## Hardware integration

Проверить Group и Scene на нескольких реальных DMX addresses.

## PASS

Нет последовательного визуального перебора Fixtures при Scene Apply.

---

# DEV-009 — Art-Net protocol core

## Цель

Реализовать и полностью unit-test Art-Net parser/state machine до подключения реального UDP runtime.

## Реализовать

- Art-Net 4 constants;
- UDP packet structures без unsafe cast сетевого payload;
- `ArtDmx`;
- `ArtPoll`;
- `ArtPollReply`;
- signature/opcode/protocol validation;
- Port-Address;
- Universe config 0..32767;
- ArtDmx Length even 2..512;
- persistent `artnet_state[512]`;
- short packet Hold Last;
- long packet truncation to physical slot_count;
- new channels zero-initialized when slot_count grows;
- Sequence:
  - 0 disables ordering;
  - normal increment;
  - rollover;
  - stale packet rejection;
  - reset after LOST/new source;
- source state:
  - WAITING;
  - ACTIVE;
  - LOST;
  - CONFLICT;
- active-source IP lock;
- release after timeout;
- no HTP/LTP merge.

## Tests

Использовать deterministic packet fixtures:

- valid/invalid header;
- invalid opcode;
- protocol version;
- odd Length;
- too short/too long payload;
- wrong universe;
- Length 10 into 40;
- Length 100 into 40;
- Sequence rollover;
- reboot-like sequence reset;
- second source conflict;
- lock release after LOST.

## PASS

Art-Net core не требует socket или real time для unit tests.

---

# DEV-010 — Art-Net runtime, reliability and Source switching

## Цель

Подключить Art-Net core к UDP 6454 и доказать автоматическое восстановление реальной связи.

## Реализовать

Art-Net worker:

- IPv4 UDP 6454;
- bind/rebind;
- ArtPoll receive;
- ArtPollReply;
- ArtDmx receive;
- 3 s LOST timeout;
- socket auto-recovery;
- diagnostics;
- active source IP;
- conflict source IP;
- `last_packet_age`;
- Hold Last.

Source selector:

- MQTT / ART-NET;
- switch только между DMX frames;
- оба inputs работают постоянно;
- ART-NET без первого valid ArtDmx:
  - сохранить текущий physical output до появления первого Art-Net snapshot;
- ART-NET LOST:
  - Hold Last;
  - Source остаётся ART-NET;
- возврат Art-Net:
  - автоматический;
  - без restart.

## Software test source

На ноутбуке подобрать отдельное Art-Net приложение/контроллер и зафиксировать его название/версию в `PROJECT_STATE.md` перед hardware/network acceptance.

## Reliability tests

При активном Art-Net:

- disconnect Ethernet ~0.5 s;
- 5 s;
- 30 s;
- restart Art-Net application;
- power/off-on source where applicable;
- IP change;
- WB network down/up;
- repeated disconnect/reconnect;
- second Art-Net source conflict if возможно воспроизвести.

Во всех случаях:

- `dmxwb` process не требует restart;
- physical DMX продолжает Hold Last при loss;
- после возврата управление автоматически возобновляется;
- Source не меняется сам;
- sequence/source lock не препятствуют recovery.

## PASS

Главный критерий:

> после случайного временного обрыва Art-Net оператор не выполняет никаких действий для восстановления DMXWB.

---

# DEV-011 — static MQTT-only Web UI

## Цель

Реализовать полный статический интерфейс настройки/управления поверх уже стабильного MQTT API.

## Реализовать

```text
www/dmxwb/
    index.html
    app.js
    model.js
    mqtt-client.js
    styles.css
```

Без Node.js runtime/build step и без внешних интернет-зависимостей.

Разделы:

- Управление;
- Светильники и группы;
- Сцены;
- Настройки.

Fixture UI:

- Name;
- Power;
- Color;
- R/G/B;
- Brightness;
- Temperature;
- Reset.

Constructor:

- Fixture Count;
- Start Address;
- Names.

Groups:

- create;
- rename;
- members;
- delete;
- controls.

Scenes:

- create from current state;
- apply;
- overwrite;
- rename;
- delete.

Settings:

- DMX port;
- Refresh Rate;
- Art-Net Universe;
- Source;
- DMX/Art-Net diagnostics.

MQTT:

- WebSocket `/mqtt`;
- automatic reconnect;
- retained resync;
- no command replay;
- live controls immediate;
- structural config via Apply;
- `request_id + expected_revision`;
- validation errors displayed;
- 20–30 publishes/s throttle per slider;
- final slider value always sent.

## Tests

- offline/static load;
- no Node.js runtime;
- MQTT reconnect;
- two browser tabs/revision conflict;
- invalid config rejection;
- Fixture controls;
- Group controls;
- Scene operations;
- Source switching;
- diagnostic state updates.

## PASS

Web не содержит никакого прямого serial/file/systemd API и не является обязательным для продолжения DMX при закрытом браузере.

---

# DEV-012 — systemd, diagnostics and fully offline deployment

## Цель

Оформить приложение как штатно устанавливаемый и самовосстанавливающийся daemon Wiren Board и доказать, что production bundle устанавливается полностью офлайн.

## Реализовать

- `deploy/dmxwb.service`;
- `Type=simple`;
- `Restart=on-failure`;
- `RestartSec=2s`;
- `deploy/install_wirenboard.sh`;
- финальный локальный installation bundle: готовый ARM64 `dmxwb`, static web, systemd unit, installer и все требуемые локальные runtime-файлы;
- installer не выполняет `apt update`, online `apt install`, `git clone`, `curl`, `wget`, `npm` или другие интернет-загрузки;
- installer не требует C++ compiler, CMake или Node.js на целевом Wiren Board;
- создание:
  - `/etc/dmxwb`;
  - `/var/lib/dmxwb`;
  - `/var/www/dmxwb`;
- permissions;
- journald logging;
- graceful SIGTERM;
- MQTT LWT;
- startup/reconnect ordering;
- diagnostics согласно ТЗ;
- metadata hidden для Fixture/Group/Scene;
- в штатном WB UI видны только:
  - DMXWB Status;
  - Source.

## Tests

- fresh install при физически недоступном внешнем интернете;
- установка только из локального bundle и отсутствие сетевых загрузок;
- отсутствие requirement на C++/CMake/Node.js toolchain на целевом WB;
- start;
- stop;
- restart;
- daemon crash -> systemd recovery;
- MQTT broker restart;
- serial recoverable error;
- reboot WB;
- state/config restored;
- web доступен после reboot по локальной LAN;
- backend подключается к локальному Mosquitto;
- базовый physical DMX output работает после offline install;
- Art-Net принимается из локальной сети;
- standard WB UI не засорён скрытыми controls.

## PASS

Обычные recoverable subsystem errors исправляются самим приложением, systemd restart нужен только при реальном падении процесса, а чистая установка, reboot и базовая работа DMXWB проходят без доступа во внешний интернет.

---

# DEV-013 — full integration, offline installation and final acceptance

## Цель

Доказать, что реализовано всё `TECHNICAL_SPEC.md` как единая система.

## Перед началом

Выполнить requirements traceability review:

- каждый MUST/утверждённый пункт ТЗ связан с кодом;
- каждый критический алгоритм связан с тестом;
- нет оставшихся временных diagnostic shortcuts в production path.

## Full functional acceptance

Проверить:

### DMX

- default `/dev/ttyRS485-1`;
- dynamic slot_count;
- Start Address !=1;
- 10/30/44 Hz в физически допустимых конфигурациях;
- continuous output;
- no visible flicker from DMXWB.

### Fixture

- R/G/B;
- Color;
- Temperature;
- Brightness;
- Power restore;
- Reset;
- factual MQTT state.

### Groups

- multiple memberships;
- all controls;
- factual Power;
- reset.

### Scenes

- create;
- apply;
- overwrite;
- rename;
- delete;
- atomic visual apply.

### MQTT

- broker restart/reconnect;
- retained state;
- retained command rejection;
- standard WB UI system device.

### Art-Net

- raw channel mapping;
- short/long packets;
- Source switching;
- loss/recovery;
- IP change;
- conflict;
- Hold Last.

### Web

- all management functions;
- reconnect;
- revision conflict;
- no external runtime dependencies.

### Persistence

- service restart;
- WB reboot;
- config corruption test;
- state corruption test;
- no damaged atomic file after simulated interrupted update where safely testable.

### Offline installation

На поддерживаемом Wiren Board при физически недоступном внешнем интернете:

- установить только из финального локального bundle;
- подтвердить отсутствие online package/download steps;
- перезагрузить WB;
- подтвердить запуск `dmxwb` через systemd;
- открыть Web по локальной LAN;
- подтвердить локальный MQTT;
- подтвердить базовый DMX output;
- подтвердить Art-Net input из локальной сети.

## 24-hour test

Не менее 24 часов непрерывной работы.

В ходе теста периодически:

- менять Fixture RGB/Temperature/Brightness;
- выполнять Group commands;
- применять Scenes;
- переключать Source;
- использовать Art-Net;
- делать network disconnect/reconnect;
- перезапускать MQTT broker;
- контролировать process uptime/restarts;
- проверять отсутствие видимого flicker.

## FINAL PASS

Проект считается реализованным по ТЗ, когда:

- все automated tests PASS;
- все hardware/network acceptance tests PASS;
- offline installation acceptance PASS;
- 24-hour test PASS;
- не требуется ручной restart после recoverable Art-Net/MQTT/serial failure;
- физический DMX и Art-Net выполняют главную функцию проекта на реальном Wiren Board;
- документация соответствует реальному приложению;
- `PROJECT_STATE.md` содержит итоговую проверенную конфигурацию и результаты acceptance.

---

## 3. Документация по этапам

После каждого PASS обновлять минимум:

```text
docs/PROJECT_STATE.md
```

Обновлять `README.md`, если изменились реальные команды сборки/установки/запуска или пользовательская точка входа.

Обновлять `docs/TECHNICAL_SPEC.md` только при согласованном изменении требований.

Обновлять `docs/ROADMAP.md`, если изменился порядок/границы будущих gates.

Не создавать отдельные конкурирующие архитектурные документы без реальной необходимости.

---

## 4. Принцип остановки

Если текущий gate выявляет фундаментальную проблему, следующий gate не начинается.

Особенно:

```text
DEV-003 physical DMX FAIL
-> не начинать Fixture/MQTT/Art-Net

DEV-004 unstable continuous DMX
-> не начинать application layers

DEV-010 Art-Net recovery FAIL
-> не считать сетевой режим готовым и не маскировать проблему Web UI
```

Цель roadmap — не скорость прохождения этапов, а сохранение локальности ошибок и доказуемость каждого слоя.

---

## 5. Как продолжать после каждого SHA

После получения от пользователя нового полного SHA следующая модель действует механически:

1. читает `AGENTS.md`;
2. читает `PROJECT_STATE.md` и новый SHA пользователя;
3. находит текущий/следующий gate в этом roadmap;
4. скачивает необходимые файлы с GitHub именно на этом SHA;
5. выполняет только scope выбранного gate;
6. обновляет относящуюся к шагу документацию и `PROJECT_STATE.md`;
7. упаковывает финальные файлы в root-relative ZIP;
8. передаёт пользователю handoff строго по формату `AGENTS.md`;
9. при ошибке остаётся на том же gate;
10. при новом SHA переходит к следующему gate.

Нельзя перескакивать через hardware gates ради ускорения UI/MQTT/Art-Net разработки. Конечный критерий — полностью рабочее управление реальным DMX-освещением и надёжное внешнее управление по Art-Net на Wiren Board.
