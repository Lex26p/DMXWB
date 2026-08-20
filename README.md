# DMXWB

DMXWB — специализированное C++20-приложение, расширяющее возможности контроллеров **серии Wiren Board 8 (WB8)** поддержкой физического DMX512 через встроенный RS-485.

Приложение проектируется с двумя независимыми источниками управления:

- **WB MQTT** — управление RGBW-светильниками, группами и сценами как часть экосистемы Wiren Board;
- **Art-Net** — прямое управление DMX-каналами по Ethernet.

Источник выбирается явно. Автоматического переключения между WB MQTT и Art-Net нет.

DMXWB **не заменяет Wiren Board**. Приложение использует штатную инфраструктуру WB согласно `docs/TECHNICAL_SPEC.md`.

Финальная установка на WB8 должна выполняться полностью офлайн. Интернет может использоваться на development-ноутбуке для подготовки build environment; контроллеру интернет для build/deployment/runtime не требуется.

## Текущий статус

Последний фактически завершённый engineering gate:

```text
DEV-003 — physical DMX transport proof
```

DEV-003 состоит из:

```text
DEV-003A — laptop -> WB8 target build enablement
DEV-003B — physical DMX transport proof
```

Оба подшага получили PASS на реальном WB8.

База hardware-test шага:

```text
bc359169e95a118f3f999854a9cd9511258dd76c
Enable verified WB8 ARM64 cross build
```

Репозиторное закрытие DEV-003 выполняется commit-ом, содержащим:

```text
docs/DEV003A_TARGET_REPORT.txt
docs/DEV003B_HARDWARE_REPORT.txt
tools/wb8/run_dev003b_physical_test.sh
README.md
docs/PROJECT_STATE.md
```

Следующий engineering gate после этого closure commit:

```text
DEV-004 — continuous DMX engine, timing and serial recovery
```

## Подтверждённый DEV-003 target

Фактическая hardware acceptance конфигурация DEV-003:

```text
Controller:       Wiren Board rev. 8.5.1 (T507)
Architecture:     aarch64 / arm64
OS:               Debian 11 Bullseye
WB release:       wb-2606 stable
Kernel:           6.8.0-wb160
glibc:            2.31
DMX port:         /dev/ttyRS485-1 -> ttyS2
Build compiler:   Bullseye aarch64-linux-gnu-g++ 10.2.1
Artifact glibc:   maximum required symbol GLIBC_2.17
GNU C++ runtime:  statically linked
Docker:           not used
```

Конкретная тестовая модель не становится единственной поддерживаемой моделью: target проекта остаётся **серия WB8**.

## Подтверждённый физический DMX transport

DEV-003B подтвердил на реальном RGBW fixture со стартовым DMX-адресом `1`:

```text
all-off  -> PASS
red      -> PASS
green    -> PASS
blue     -> PASS
white    -> PASS
all-on   -> PASS
```

Для всех patterns подтверждено:

- правильное соответствие RGBW каналов;
- отсутствие заметного flicker во время diagnostic burst;
- `250000 8N2`;
- BREAK proof `38400 + 0x00 + drain -> 250000 8N2`;
- Start Code `0x00`;
- serial закрывается после каждого запуска;
- порт повторно открывается между независимыми процессами;
- финальный safe `all-off` проходит;
- kernel/WBEC patch не требуется.

Фактический отчёт хранится в:

```text
docs/DEV003B_HARDWARE_REPORT.txt
```

## Текущая архитектура

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
              DmxOutput
                  |
          built-in RS-485
                  |
                DMX512
```

На текущем состоянии `DmxOutput` production worker ещё не реализован. Это задача DEV-004.

## Среда разработки

Основная среда:

```text
Windows
Visual Studio 2026
C:\Projects\DMXWB
```

На том же ноутбуке используется локальный Linux.

Разделение:

- **Windows / Visual Studio 2026** — host development/tests;
- **локальный Linux** — Linux-specific tests и target build;
- **WB8** — runtime/hardware/integration target;
- **Docker не используется**.

Минимальная версия CMake: `3.18`. Проект использует C++20.

### Windows / Visual Studio 2026

```powershell
cmake -S . -B build -DBUILD_TESTING=ON -DDMXWB_WARNINGS_AS_ERRORS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
.\build\Debug\dmxwb.exe --version
.\build\Debug\dmxwb_tests.exe
```

На Windows hardware serial backend намеренно недоступен.

### Local Linux host verification

```sh
cmake -S . -B build -DBUILD_TESTING=ON -DDMXWB_WARNINGS_AS_ERRORS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## WB8 target build

DEV-003A установил воспроизводимый non-Docker build path:

```text
local Linux laptop (amd64)
    -> Debian 11 Bullseye amd64 build rootfs
    -> crossbuild-essential-arm64
    -> aarch64-linux-gnu-g++ 10
    -> ARM64 target ELF
    -> direct SSH/SCP transfer to WB8
```

Подготовка rootfs:

```sh
sudo apt update
sudo apt install -y debootstrap binutils file openssh-client
bash tools/wb8/setup_bullseye_arm64_rootfs.sh
```

Сборка:

```sh
bash tools/wb8/build_bullseye_arm64.sh
```

Результат:

```text
artifacts/wb8-bullseye-arm64/dmxwb
```

`artifacts/` — локальный build output и не предназначен для commit.

Target smoke test:

```sh
bash tools/wb8/verify_on_target.sh root@REAL_WB_IP
```

Он сохраняет:

```text
docs/DEV003A_TARGET_REPORT.txt
```

## DEV-003 diagnostic mode

Diagnostic CLI остаётся полезным инструментом hardware regression:

```sh
./dmxwb --dmx-test red --port /dev/ttyRS485-1 --start-channel 1 --frames 120
```

Patterns:

```text
all-off
red
green
blue
white
all-on
```

Перед прямой работой выбранный RS-485 порт должен быть освобождён от штатного serial driver Wiren Board.

Automation helper для полного физического smoke test:

```sh
bash tools/wb8/run_dev003b_physical_test.sh root@REAL_WB_IP 1
```

Этот diagnostic не является production scheduler.

## Следующий gate — DEV-004

DEV-004 должен превратить доказанный transport в независимый непрерывный DMX output engine:

- один `DmxOutput` worker — единственный владелец serial fd;
- continuous BREAK + frame loop;
- absolute frame-start scheduling;
- configurable refresh `10..44 Hz`, default `30 Hz`;
- validation физически достижимого refresh;
- принятие нового whole snapshot только между кадрами;
- runtime refresh change без закрытия serial, когда это возможно;
- serial error detection;
- close/reopen/retry;
- recovery с актуального snapshot;
- timing/recovery diagnostics and counters.

MQTT, Fixture и Art-Net по-прежнему не должны опережать этот gate.

## Источник истины и порядок работы

**Источник истины — актуальный репозиторий DMXWB.**

Документы читать в порядке:

1. [`AGENTS.md`](AGENTS.md)
2. [`docs/PROJECT_STATE.md`](docs/PROJECT_STATE.md)
3. [`docs/TECHNICAL_SPEC.md`](docs/TECHNICAL_SPEC.md)
4. [`docs/ROADMAP.md`](docs/ROADMAP.md)

Пользователь самостоятельно выполняет commit/push и присылает новый полный SHA после завершения шага.
