# DMXWB

DMXWB — специализированная программная подсистема Wiren Board 8.5.1 на C++20, которая формирует одну физическую линию DMX512 через встроенный RS-485 и поддерживает два независимых источника управления:

- **WB MQTT** — управление RGBW-светильниками, группами и сценами;
- **Art-Net** — прямое управление DMX-каналами по Ethernet.

Источник выбирается явно через MQTT. Автоматического переключения между WB MQTT и Art-Net нет. Главная задача DMXWB — стабильное управление реальным DMX-освещением и приём внешнего управления по Art-Net.

DMXWB является частью программной среды контроллера, а не отдельной универсальной платформой. Эксплуатация предполагается в доверенной локальной LAN; собственная authentication/authorization/ACL-модель не входит в scope приложения. Финальная установка на Wiren Board должна выполняться полностью офлайн, без интернет-загрузок.

## Текущий статус

`DEV-001` подтверждён commit SHA `6704b01ac25a44b5174178f52bdc7158d0295ef3`.

Выполняется `DEV-002 — DMX core types and deterministic frame model`. На этом этапе добавлены hardware-independent типы, которые позднее будут передаваться физическому DMX worker целым immutable snapshot:

- `DmxSnapshot` с максимум 512 DMX channels;
- `slot_count` и generation;
- безопасная one-based индексация channel `1..512`;
- helper расчёта последнего физического slot;
- отдельная модель Start Code `0x00` и channel payload;
- atomic publication `shared_ptr<const DmxSnapshot>` без частично обновлённого snapshot;
- abstraction монотонных часов для будущего deterministic scheduler;
- deterministic host unit tests.

Serial/termios/BREAK, непрерывный DMX worker, Fixture color algorithms, MQTT и Art-Net в `DEV-002` намеренно отсутствуют.

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

## Сборка DEV-002

Требуется CMake 3.20+ и C++20 compiler. Внешние библиотеки на этом этапе не используются.

Windows / Visual Studio:

```powershell
cmake -S . -B build -DBUILD_TESTING=ON -DDMXWB_WARNINGS_AS_ERRORS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
.\build\Debug\dmxwb.exe --version
.\build\Debug\dmxwb_tests.exe
```

Для single-config генератора (например Ninja) executable обычно находится непосредственно в `build/`.

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
