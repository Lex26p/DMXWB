# DMXWB

DMXWB — специализированное C++20-приложение, расширяющее возможности контроллеров **серии Wiren Board 8 (WB8)** поддержкой физического DMX512 через встроенный RS-485.

Приложение поддерживает два независимых источника управления:

- **WB MQTT** — управление RGBW-светильниками, группами и сценами как часть экосистемы Wiren Board;
- **Art-Net** — прямое управление DMX-каналами по Ethernet.

Источник выбирается явно через MQTT. Автоматического переключения между WB MQTT и Art-Net нет. Главная задача DMXWB — стабильное управление реальным DMX-освещением и приём внешнего управления по Art-Net.

DMXWB **не заменяет Wiren Board и не является самостоятельной SCADA или универсальной платформой автоматизации**. Приложение использует штатную инфраструктуру WB согласно архитектуре проекта. Эксплуатация предполагается в доверенной локальной LAN; собственная authentication/authorization/ACL-модель не входит в scope приложения.

Финальная установка на WB8 должна выполняться полностью офлайн, без интернет-загрузок. Интернет может использоваться на development-ноутбуке для подготовки build environment; это не является runtime/deployment dependency контроллера.

## Текущий статус

Последний завершённый engineering gate:

```text
DEV-002 — DMX core types and deterministic frame model
6b6e5b8329bbf1d9c893205d60427974e8e59bd5
```

Текущий engineering gate:

```text
DEV-003 — physical DMX transport proof
```

Он состоит из двух последовательных подшагов:

```text
DEV-003A — laptop -> WB8 target build enablement
DEV-003B — physical DMX transport proof
```

В коде DEV-003 уже присутствуют:

- Linux `DmxTransport` для встроенного RS-485;
- default `/dev/ttyRS485-1`;
- data mode `250000 8N2`;
- BREAK proof method `38400 + 0x00 + drain -> 250000 8N2`;
- Start Code `0x00` + immutable DMX payload;
- diagnostic patterns `all-off/red/green/blue/white/all-on`;
- host tests diagnostic snapshot mapping.

DEV-003 не считается завершённым до target-build proof и физического теста на реальном WB8 с RGBW fixture.

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

На ноутбуке также используется локальная Linux-среда.

Принятое разделение:

- **Windows / Visual Studio 2026** — основная host development/test среда;
- **локальный Linux** — Linux-specific tests и сборка WB8 target artifact;
- **контроллер WB8** — runtime/hardware/integration target.

**Docker не используется** ни для build, ни для runtime, ни для deployment, ни для tests.

### Windows / Visual Studio 2026 — host build / tests

Минимальная версия CMake: **3.18**. Проект по-прежнему собирается как C++20.

```powershell
cmake -S . -B build -DBUILD_TESTING=ON -DDMXWB_WARNINGS_AS_ERRORS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
.\build\Debug\dmxwb.exe --version
.\build\Debug\dmxwb_tests.exe
```

На Windows hardware serial backend намеренно недоступен.

### Local Linux — обычные host tests

```sh
cmake -S . -B build -DBUILD_TESTING=ON -DDMXWB_WARNINGS_AS_ERRORS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## DEV-003A — WB8 target build без Docker

### Compatibility baseline

Для DEV-003A используется Debian 11 Bullseye `arm64` как минимальный userspace build baseline для серии WB8.

Причина выбора: `wb-2606` является последним WB release на Debian 11, а более новые WB8 могут работать на Debian 13. Бинарник, собранный против более старого glibc baseline, проверяется на фактическом контроллере перед hardware test.

GNU C++ runtime (`libstdc++` и `libgcc`) для target artifact линкуется статически; системная `glibc` остаётся динамической. Build script проверяет, что бинарник не требует glibc новее `2.31`.

### Подготовка Bullseye ARM64 cross-build environment

Build environment создаётся локально на Linux без Docker и без QEMU:

```text
local Ubuntu/Linux amd64
    -> native Debian 11 Bullseye amd64 rootfs (debootstrap)
    -> Bullseye crossbuild-essential-arm64
    -> aarch64-linux-gnu-g++ 10
    -> ARM64 target ELF
```

Такой подход не зависит от механизма `binfmt` хостовой Ubuntu и при этом использует Bullseye ARM64 cross libc/toolchain как compatibility baseline.

Host prerequisites для Debian/Ubuntu:

```sh
sudo apt update
sudo apt install -y debootstrap binutils file openssh-client
```

Создание build rootfs выполняется один раз:

```sh
bash tools/wb8/setup_bullseye_arm64_rootfs.sh
```

Несмотря на историческое имя script, rootfs сам является `amd64`; ARM64 получается штатным Bullseye cross compiler.

Default rootfs:

```text
/opt/dmxwb/wb8-bullseye-cross-arm64
```

Можно изменить через:

```sh
DMXWB_WB8_ROOTFS=/custom/path bash tools/wb8/setup_bullseye_arm64_rootfs.sh
```

### Сборка target artifact

```sh
bash tools/wb8/build_bullseye_arm64.sh
```

Скрипт:

1. выполняет обычный Linux host build + CTest;
2. копирует текущие исходники в Bullseye build rootfs, исключая `.git`, `.vs`, build trees и `artifacts`;
3. cross-компилирует Release target через `aarch64-linux-gnu-g++`;
4. включает warnings-as-errors;
5. собирает target binary со статическим GNU C++ runtime;
6. проверяет AArch64 ELF и glibc symbol baseline.

Cross-built unit-test executable на ноутбуке не запускается: deterministic tests выполняются нативно на Windows/Linux, а сам ARM64 binary проверяется запуском на реальном WB8.

Результат:

```text
artifacts/wb8-bullseye-arm64/dmxwb
```

`artifacts/` не предназначен для commit в Git.

### Target probe и CLI smoke test

После сборки:

```sh
bash tools/wb8/verify_on_target.sh root@192.168.1.50
```

IP в примере нужно заменить на фактический адрес контроллера. Скрипт копирует `dmxwb` и target probe во временный каталог WB8, проверяет target environment, запускает:

```text
dmxwb --version
dmxwb --help
```

и сохраняет воспроизводимый отчёт:

```text
docs/DEV003A_TARGET_REPORT.txt
```

Этот отчёт является частью результата DEV-003A и должен быть закоммичен после PASS.

Физические DMX patterns относятся уже к DEV-003B и не запускаются автоматически этим скриптом.

## Hardware diagnostic DEV-003B

После PASS DEV-003A diagnostic example для RGBW fixture со стартовым DMX-адресом 1:

```sh
./dmxwb --dmx-test red --port /dev/ttyRS485-1 --start-channel 1 --frames 120
```

Перед hardware test `/dev/ttyRS485-1` должен быть освобождён от штатного serial driver Wiren Board.

## Источник истины и порядок работы

**Источник истины — актуальный репозиторий DMXWB.**

Документы читать в таком порядке:

1. [`AGENTS.md`](AGENTS.md) — правила совместной разработки и передачи изменений.
2. [`docs/PROJECT_STATE.md`](docs/PROJECT_STATE.md) — текущее состояние и ближайший шаг.
3. [`docs/TECHNICAL_SPEC.md`](docs/TECHNICAL_SPEC.md) — требования к конечному приложению.
4. [`docs/ROADMAP.md`](docs/ROADMAP.md) — последовательность gates и критерии PASS.

Пользователь самостоятельно выполняет commit/push и присылает новый полный SHA после успешной проверки шага.

Исторические исследования MOD1/WBEC/custom kernel не являются источником истины текущего проекта.
