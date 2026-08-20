# DMXWB

DMXWB — специализированная программная подсистема Wiren Board 8.5.1 на C++20, которая формирует одну физическую линию DMX512 через встроенный RS-485 и поддерживает два независимых источника управления:

- **WB MQTT** — управление RGBW-светильниками, группами и сценами;
- **Art-Net** — прямое управление DMX-каналами по Ethernet.

Источник выбирается явно через MQTT. Автоматического переключения между WB MQTT и Art-Net нет. Главная задача DMXWB — стабильное управление реальным DMX-освещением и приём внешнего управления по Art-Net.

DMXWB является частью программной среды контроллера, а не отдельной универсальной платформой. Эксплуатация предполагается в доверенной локальной LAN; собственная authentication/authorization/ACL-модель не входит в scope приложения. Финальная установка на Wiren Board должна выполняться полностью офлайн, без интернет-загрузок.

## Текущий статус

`DEV-002` подтверждён commit SHA `6b6e5b8329bbf1d9c893205d60427974e8e59bd5`.

Выполняется первый hardware gate: **`DEV-003 — physical DMX transport proof`**.

Добавлены:

- Linux `DmxTransport` для встроенного RS-485;
- default `/dev/ttyRS485-1`;
- data mode `250000 8N2`;
- WB BREAK proof method `38400 + 0x00 + drain -> 250000 8N2`;
- Start Code `0x00` + immutable DMX payload;
- diagnostic patterns `all-off/red/green/blue/white/all-on`;
- выбор start channel для реального RGBW fixture;
- host tests diagnostic snapshot mapping.

`DEV-003` не считается завершённым до проверки на реальном Wiren Board и реальном RGBW-светильнике.

Continuous scheduler, refresh control, serial auto-recovery, Fixture model, MQTT и Art-Net пока намеренно отсутствуют.

## Текущая целевая архитектура

```text
Static Web
    |
MQTT WebSocket
    |
Mosquitto
    |
DMXWB C++20
  |        |
 MQTT   Art-Net
   \      /
   snapshots
      |
 DMX output
      |
/dev/ttyRS485-1
```

Порт по умолчанию: `/dev/ttyRS485-1`.

## Host build / tests

Требуется CMake 3.20+ и C++20 compiler. Внешние библиотеки на текущем этапе не используются.

Windows / Visual Studio:

```powershell
cmake -S . -B build -DBUILD_TESTING=ON -DDMXWB_WARNINGS_AS_ERRORS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
.\build\Debug\dmxwb.exe --version
.\build\Debug\dmxwb_tests.exe
```

На Windows hardware serial backend намеренно недоступен; физический тест выполняется только на Linux/Wiren Board.

Linux / Wiren Board development build:

```sh
cmake -S . -B build -DBUILD_TESTING=ON -DDMXWB_WARNINGS_AS_ERRORS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Diagnostic example для RGBW fixture со стартовым DMX-адресом 1:

```sh
./build/dmxwb --dmx-test red --port /dev/ttyRS485-1 --start-channel 1 --frames 120
```

Перед hardware test `/dev/ttyRS485-1` должен быть освобождён от штатного serial driver Wiren Board.

## Источники истины

Перед началом любой работы необходимо читать документы в таком порядке:

1. [`AGENTS.md`](AGENTS.md) — правила совместной разработки и передачи изменений.
2. [`docs/PROJECT_STATE.md`](docs/PROJECT_STATE.md) — текущее состояние и ближайший шаг.
3. [`docs/TECHNICAL_SPEC.md`](docs/TECHNICAL_SPEC.md) — утверждённые требования к конечному приложению.
4. [`docs/ROADMAP.md`](docs/ROADMAP.md) — последовательность gates и критерии PASS.

Если требования меняются в обсуждении, соответствующие документы должны быть обновлены вместе с кодом в том же шаге.

## Разработка

Проект разрабатывается небольшими проверяемыми этапами. Каждый значимый этап должен иметь:

- точную базовую Git SHA;
- код и документацию в одном изменении;
- конкретные команды проверки;
- явный результат PASS/FAIL;
- новый commit SHA от пользователя перед продолжением следующего этапа.

Исторические исследования MOD1/WBEC/Linux kernel не являются источником истины текущего проекта и намеренно удалены из рабочего дерева.
