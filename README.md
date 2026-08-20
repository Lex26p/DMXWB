# DMXWB

DMXWB — специализированное C++20-приложение, расширяющее возможности контроллеров **серии Wiren Board 8 (WB8)** поддержкой физического DMX512 через встроенный RS-485.

Приложение поддерживает два независимых источника управления:

- **WB MQTT** — управление RGBW-светильниками, группами и сценами как часть экосистемы Wiren Board;
- **Art-Net** — прямое управление DMX-каналами по Ethernet.

Источник выбирается явно через MQTT. Автоматического переключения между WB MQTT и Art-Net нет. Главная задача DMXWB — стабильное управление реальным DMX-освещением и приём внешнего управления по Art-Net.

DMXWB **не заменяет Wiren Board и не является самостоятельной SCADA или универсальной платформой автоматизации**. Приложение использует штатную инфраструктуру WB согласно архитектуре проекта. Эксплуатация предполагается в доверенной локальной LAN; собственная authentication/authorization/ACL-модель не входит в scope приложения.

Финальная установка на WB8 должна выполняться полностью офлайн, без интернет-загрузок.

## Текущий статус

Текущий `master` на момент последней актуализации документации:

```text
bbf9f0d334564fa8ae006f9ffd3fa756aefe5cc7
```

Он содержит реализацию пакета **`DEV-003 — physical DMX transport proof`**.

Последний завершённый engineering gate:

```text
DEV-002 — DMX core types and deterministic frame model
6b6e5b8329bbf1d9c893205d60427974e8e59bd5
```

DEV-003 не считается завершённым до проверки на реальном контроллере серии WB8 и реальном RGBW-светильнике.

В DEV-003 добавлены:

- Linux `DmxTransport` для встроенного RS-485;
- default `/dev/ttyRS485-1`;
- data mode `250000 8N2`;
- WB BREAK proof method `38400 + 0x00 + drain -> 250000 8N2`;
- Start Code `0x00` + immutable DMX payload;
- diagnostic patterns `all-off/red/green/blue/white/all-on`;
- выбор start channel для реального RGBW fixture;
- host tests diagnostic snapshot mapping.

Continuous scheduler, refresh control, serial auto-recovery, Fixture model, MQTT и Art-Net пока намеренно отсутствуют.

## Текущая целевая архитектура

```text
          Wiren Board ecosystem
                 |
             Mosquitto
                 |
             DMXWB C++20
             /         \
        WB MQTT       Art-Net
             \         /
              snapshots
                  |
              DMX output
                  |
          built-in RS-485
                  |
                DMX512
```

Порт по умолчанию: `/dev/ttyRS485-1`.

## Среда разработки

Основная пользовательская среда:

```text
Windows
Visual Studio 2026
C:\Projects\DMXWB
```

На ноутбуке также установлена локальная Linux-среда.

Принятое разделение:

- **Windows / Visual Studio 2026** — основная host development/test среда;
- **локальный Linux** — Linux-specific build/tests и подготовка целевой сборки для WB8;
- **контроллер WB8** — runtime/hardware/integration target.

Production binary должен собираться на ноутбуке с использованием подходящей Linux/cross-build среды, а не требовать компиляции исходников на WB8.

**Docker не используется** ни как build, ни как runtime, ни как deployment dependency.

### Windows / Visual Studio 2026 — host build / tests

Требуется CMake 3.20+ и C++20 compiler.

```powershell
cmake -S . -B build -DBUILD_TESTING=ON -DDMXWB_WARNINGS_AS_ERRORS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
.\build\Debug\dmxwb.exe --version
.\build\Debug\dmxwb_tests.exe
```

На Windows hardware serial backend намеренно недоступен.

### Local Linux — Linux-specific build / tests

```sh
cmake -S . -B build -DBUILD_TESTING=ON -DDMXWB_WARNINGS_AS_ERRORS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Конкретный production cross-build/toolchain для WB8 должен быть зафиксирован отдельным этапом до финального deployment bundle.

### Hardware diagnostic on WB8

После подготовки подходящего бинарника diagnostic example для RGBW fixture со стартовым DMX-адресом 1:

```sh
./dmxwb --dmx-test red --port /dev/ttyRS485-1 --start-channel 1 --frames 120
```

Перед hardware test `/dev/ttyRS485-1` должен быть освобождён от штатного serial driver Wiren Board.

## Источник истины и порядок работы

**Источник истины — актуальный репозиторий DMXWB.**

Перед началом любой работы ассистент читает текущее состояние репозитория и необходимые файлы.

Документы внутри репозитория читать в таком порядке:

1. [`AGENTS.md`](AGENTS.md) — правила совместной разработки и передачи изменений.
2. [`docs/PROJECT_STATE.md`](docs/PROJECT_STATE.md) — текущее состояние и ближайший шаг.
3. [`docs/TECHNICAL_SPEC.md`](docs/TECHNICAL_SPEC.md) — требования к конечному приложению.
4. [`docs/ROADMAP.md`](docs/ROADMAP.md) — последовательность gates и критерии PASS.

Если требования или процесс меняются, соответствующая документация обновляется отдельным проверяемым шагом.

## Разработка

Проект разрабатывается небольшими шагами.

Обычный handoff:

1. краткое описание шага;
2. получение необходимых файлов из актуального GitHub и выполнение работы;
3. ZIP с финальными изменёнными/новыми файлами;
4. команда PowerShell для распаковки поверх `C:\Projects\DMXWB`;
5. команды сборки/запуска, только если они нужны;
6. конкретная пользовательская проверка, если она нужна;
7. Git-команды.

Пользователь самостоятельно выполняет commit/push и присылает новый полный SHA.

Новый SHA означает завершение текущего шага и разрешает переход к следующему, если он предусмотрен.

Полные правила находятся в [`AGENTS.md`](AGENTS.md).

Исторические исследования MOD1/WBEC/custom kernel не являются источником истины текущего проекта.
