# PROJECT_STATE

**Last updated:** 2026-08-20

## Repository base for current step

Источник истины проекта — актуальное состояние репозитория.

База DEV-003A package:

```text
b90cd3c0ca22b67e920828696aff5d30dccaacec
```

Commit:

```text
Align DMXWB specification and roadmap with WB8 workflow
```

Этот SHA завершил синхронизацию `TECHNICAL_SPEC.md` и `ROADMAP.md` с WB8 workflow.

## Last confirmed engineering PASS

```text
DEV-002 — DMX core types and deterministic frame model
6b6e5b8329bbf1d9c893205d60427974e8e59bd5
```

DEV-003 остаётся текущим engineering gate.

## Current phase

```text
DEV-003A — laptop -> WB8 target build enablement
```

Цель текущего шага — получить воспроизводимый target build на локальном Linux без Docker, доказать ARM64/Bullseye ABI baseline и запустить готовый binary на реальном WB8 до физического DMX test.

Физические RGBW patterns относятся к DEV-003B и в текущий шаг не входят.

## Confirmed external platform facts

На 2026-08-20 подтверждено официальными материалами:

- WB8 использует 64-bit ARM / `arm64` userspace target;
- официальный kernel build для WB8 использует `ARCH=arm64` и `CROSS_COMPILE=aarch64-linux-gnu-`;
- `wb-2606` — последний WB release на Debian 11 Bullseye;
- публичный testing Wiren Board с июля 2026 переведён на Debian 13 Trixie;
- kernel WB8 в этом переходе остаётся Linux 6.8.

Источники:

```text
https://wirenboard.com/wiki/How_To_Build_Linux_Kernel
https://wirenboard.com/en/news/public-testing-of-wiren-board-based-on-debian-13-trixie-has-been-launched-810/
```

## DEV-003A build decision

**Decided:** compatibility build baseline текущего gate — Debian 11 Bullseye ARM64 userspace.

Финальный build method текущего шага:

```text
local Linux laptop (amd64)
    -> native Debian 11 Bullseye amd64 rootfs via debootstrap
    -> Bullseye crossbuild-essential-arm64
    -> aarch64-linux-gnu-g++ 10
    -> target ARM64 ELF artifact
    -> copy to real WB8
```

Docker и QEMU/binfmt не используются.

Причина отказа от первого, незакоммиченного QEMU-варианта: на пользовательской Ubuntu `resolute` пакет `qemu-user-static` является virtual package и исходная host dependency command завершилась до установки `debootstrap`. Вместо привязки к меняющейся QEMU packaging выбран штатный Debian Bullseye ARM64 cross compiler.

Default build rootfs:

```text
/opt/dmxwb/wb8-bullseye-cross-arm64
```

Target artifact:

```text
artifacts/wb8-bullseye-arm64/dmxwb
```

`artifacts/` остаётся локальным build output и не commit-ится.

## Compatibility changes in this package

### CMake baseline

`cmake_minimum_required` снижен с `3.20` до `3.18`.

Причина: Debian 11 Bullseye содержит CMake 3.18.4, а текущий `CMakeLists.txt` не использует возможностей, требующих CMake 3.20.

Добавлена option:

```text
DMXWB_STATIC_GNU_RUNTIME
```

При GNU C++ она линкует target с:

```text
-static-libstdc++
-static-libgcc
```

Это уменьшает зависимость target artifact от версии `libstdc++` на конкретном WB8. `glibc` намеренно остаётся системной динамической зависимостью.

### Atomic shared_ptr compatibility

Текущий DEV-002 использовал:

```cpp
std::atomic<std::shared_ptr<const DmxSnapshot>>
```

Эта C++20 specialization отсутствует в старом libstdc++ Bullseye/GCC 10.

Теперь `DmxSnapshotPublisher` выбирает реализацию по доступности `__cpp_lib_atomic_shared_ptr`:

- современный libstdc++ -> `std::atomic<std::shared_ptr<...>>`;
- старый libstdc++ -> стандартные atomic free-functions для `std::shared_ptr`.

Инвариант immutable whole-snapshot publication не меняется.

## New DEV-003A tools

```text
tools/wb8/setup_bullseye_arm64_rootfs.sh
tools/wb8/build_bullseye_arm64.sh
tools/wb8/probe_target.sh
tools/wb8/verify_on_target.sh
```

### setup_bullseye_arm64_rootfs.sh

Создаёт локальный native Bullseye `amd64` build rootfs через `debootstrap` и устанавливает в него:

```text
build-essential
cmake
ca-certificates
file
binutils
crossbuild-essential-arm64
```

ARM64 emulation не требуется. Target compiler — Bullseye `aarch64-linux-gnu-g++`.

### build_bullseye_arm64.sh

- сначала выполняет native Linux build + CTest;
- переносит source tree в Bullseye build rootfs;
- cross-компилирует target через `aarch64-linux-gnu-g++`;
- включает warnings-as-errors;
- включает static GNU C++ runtime;
- выводит target artifact;
- проверяет AArch64 ELF;
- отклоняет динамическую зависимость от `libstdc++`/`libgcc_s`;
- отклоняет glibc symbol requirement новее `GLIBC_2.31`.

ARM64 test executable на build host не запускается. Target execution подтверждается на реальном WB8 через `verify_on_target.sh`.

### probe_target.sh

Собирает фактическую информацию о WB8:

```text
device model
uname architecture/kernel
dpkg architecture
glibc
/etc/os-release
wb-release
/dev/ttyRS485-* presence
```

### verify_on_target.sh

Копирует target artifact и probe на указанный WB8 через SSH, запускает `dmxwb --version` и `dmxwb --help`, затем сохраняет:

```text
docs/DEV003A_TARGET_REPORT.txt
```

Отчёт должен попасть в commit после успешного DEV-003A.

## Local assistant verification of this package

В доступной Linux среде выполнено:

- CMake configure;
- GCC 14.2 host build;
- `DMXWB_WARNINGS_AS_ERRORS=ON`;
- CTest PASS;
- `dmxwb --version` PASS;
- отдельный build с `DMXWB_STATIC_GNU_RUNTIME=ON`;
- в static-GNU-runtime build динамически требуется только системный libc/loader, без `libstdc++` и `libgcc_s`;
- `bash -n` для всех Bash scripts;
- `sh -n` для target probe;
- target probe smoke на non-WB Linux корректно показывает environment;
- build script корректно останавливается, если Bullseye rootfs ещё не подготовлен.

Не выполнено локально у ассистента:

- реальное создание Bullseye cross-build rootfs на пользовательском ноутбуке (требует host root и Debian package загрузки);
- реальный Bullseye GCC 10 ARM64 cross build на пользовательском ноутбуке;
- запуск бинарника на реальном WB8.

Эти проверки являются пользовательским PASS текущего шага.

## DEV-003A user PASS criteria

PASS только если выполнено всё:

1. `setup_bullseye_arm64_rootfs.sh` завершается успешно и показывает:
   - `build_rootfs_arch: amd64`;
   - Bullseye `aarch64-linux-gnu-g++`;
   - ARM64 cross libc package.
2. `build_bullseye_arm64.sh`:
   - native Linux CTest = PASS;
   - Bullseye ARM64 cross build завершается успешно;
   - выдаёт `artifacts/wb8-bullseye-arm64/dmxwb`;
   - ELF Machine = AArch64;
   - glibc requirement не новее `GLIBC_2.31`;
   - нет dynamic `libstdc++`/`libgcc_s` dependency.
3. `verify_on_target.sh root@REAL_WB_IP` завершается успешно.
4. `docs/DEV003A_TARGET_REPORT.txt` содержит фактические target данные и заканчивается:

```text
=== DEV-003A target execution PASS ===
```

5. На WB8 успешно выполняются `dmxwb --version` и `dmxwb --help`.

Если любой пункт FAIL — остаёмся в DEV-003A.

## Existing DEV-003 transport

Текущая physical transport реализация не изменяется данным шагом:

- `/dev/ttyRS485-1` default;
- `250000 8N2`;
- BREAK proof `38400 + 0x00 + drain`;
- Start Code `0x00`;
- fixed RGBW diagnostic patterns.

## Next step after DEV-003A PASS SHA

```text
DEV-003B — physical DMX transport proof
```

Только после target-build PASS выполняются реальные patterns на RGBW fixture.

DEV-004 остаётся запрещён до полного physical PASS всего DEV-003.
