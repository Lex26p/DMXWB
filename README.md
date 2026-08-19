# DMXWB

DMXWB — специализированная программная подсистема Wiren Board 8.5.1 на C++20, которая формирует одну физическую линию DMX512 через встроенный RS-485 и поддерживает два независимых источника управления:

- **WB MQTT** — управление RGBW-светильниками, группами и сценами;
- **Art-Net** — прямое управление DMX-каналами по Ethernet.

Источник выбирается явно через MQTT. Автоматического переключения между WB MQTT и Art-Net нет. Главная задача DMXWB — стабильное управление реальным DMX-освещением и приём внешнего управления по Art-Net.

DMXWB является частью программной среды контроллера, а не отдельной универсальной платформой. Эксплуатация предполагается в доверенной локальной LAN; собственная authentication/authorization/ACL-модель не входит в scope приложения. Финальная установка на Wiren Board должна выполняться полностью офлайн, без интернет-загрузок.

## Текущий статус

Repository reset завершён. Выполняется `DEV-001` — минимальная C++20/CMake foundation и deterministic unit-test harness без hardware side effects.

В `DEV-001` уже предусмотрены:

- production target `dmxwb`;
- отдельная библиотека `dmxwb_core` для общего hardware-independent кода;
- test target `dmxwb_tests` и CTest;
- общий namespace `dmxwb`;
- compiler warnings для MSVC/GCC/Clang;
- минимальный CLI `--help` / `--version`, не обращающийся к hardware.

Serial/DMX transport, `DmxSnapshot`, Fixture model, MQTT, Art-Net, persistence, systemd и web в этот gate намеренно не входят.

Текущая целевая архитектура:

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

## Сборка DEV-001

Требуется CMake 3.20+ и C++20 compiler. Внешние библиотеки на этом этапе не используются.

Windows / Visual Studio:

```powershell
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
.\build\Debug\dmxwb.exe --version
```

Для single-config генератора (например Ninja) последний запуск executable обычно будет `./build/dmxwb` вместо `build/Debug/dmxwb`.

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
