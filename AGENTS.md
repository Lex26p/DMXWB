# AGENTS.md — правила работы с DMXWB

Этот файл обязателен для чтения любой моделью/ассистентом **до любого изменения проекта**.

## 1. Источник истины

**Источник истины проекта — актуальное состояние репозитория DMXWB.**

Перед каждым шагом ассистент обязан получить из GitHub текущее состояние репозитория и необходимые для шага файлы.

Документы читать в порядке:

1. `AGENTS.md` — процесс совместной разработки и handoff.
2. `docs/PROJECT_STATE.md` — текущее состояние и ближайшая работа.
3. `docs/TECHNICAL_SPEC.md` — требования к конечному приложению.
4. `docs/ROADMAP.md` — порядок gates и критерии PASS.

Git history используется только для трассируемости и диагностики и не заменяет актуальные файлы.

## 2. Продуктовая модель

DMXWB — C++20 daemon, расширяющий контроллеры **серии Wiren Board 8 (WB8)** физическим DMX512, WB MQTT и Art-Net.

DMXWB не заменяет Wiren Board. Используются штатные возможности WB там, где это предусмотрено архитектурой: встроенный RS-485, Mosquitto/MQTT, systemd, nginx/web и локальная сеть.

Главные функции:

- стабильный физический DMX512 output;
- WB MQTT как логический источник Fixture/Group/Scene;
- Art-Net как прямой источник DMX-каналов;
- явный selector `WB MQTT / ART-NET`;
- persistence;
- статический MQTT-based web;
- полностью офлайн-устанавливаемый production bundle.

Эксплуатация предполагается в доверенной локальной LAN. Собственная authentication/authorization/ACL-модель DMXWB не входит в scope.

## 3. Поддерживаемая build/test среда

### Windows

Windows остаётся основной desktop/файловой средой пользователя:

```text
Project:   C:\Projects\DMXWB
Downloads: C:\Users\pereverworkki\Downloads
```

Windows может использоваться для:

- хранения и редактирования проекта;
- распаковки handoff ZIP;
- Git-команд;
- запуска WSL/local Linux.

**Windows compiler/MSVC не входит в поддерживаемую build/test matrix DMXWB.**

Не требовать и не предлагать CMake/MSVC build как PASS-критерий. Non-Linux build приложения не поддерживается.

### Local Linux / WSL на ноутбуке

Это основная development/build/test среда C++ приложения:

- native Linux CMake build;
- unit/integration tests;
- warnings-as-errors;
- sanitizers при необходимости;
- Linux/POSIX compile checks;
- подготовка WB8 target artifact;
- Bullseye ARM64 cross-build.

Поддерживаемые host compilers — GNU C++ и Clang на Linux. Production WB8 artifact собирается доказанным Bullseye `aarch64-linux-gnu-g++` toolchain.

### WB8

WB8 — runtime/hardware/integration target. На нём проверяются RS-485/DMX, MQTT, Art-Net, systemd, web и финальный acceptance.

Production workflow не требует компиляции исходников непосредственно на WB8.

### Исторические Windows-ссылки

Старые gate-описания или history в `ROADMAP.md`, где упомянут Visual Studio/MSVC, являются историческими сведениями о ранних проверках и **не создают текущего требования поддерживать Windows compiler**. Текущую platform/build policy определяет этот раздел.

### Docker

**Docker не используется** для build, test, runtime или deployment.

## 4. Роли

### Пользователь

Пользователь владеет локальным репозиторием и GitHub и самостоятельно:

- скачивает ZIP;
- распаковывает его поверх проекта;
- выполняет нужные build/run/test команды;
- при FAIL присылает полный вывод ошибки;
- при PASS выполняет Git-команды;
- делает commit/push;
- присылает новый полный SHA из `git rev-parse HEAD`.

### Ассистент

Ассистент **не пишет, не commit-ит и не push-ит напрямую в репозиторий пользователя**.

Ассистент:

- перед каждым шагом читает актуальный репозиторий;
- готовит изменения локально;
- выполняет доступные проверки;
- передаёт финальные файлы, а не patch/diff;
- упаковывает только изменённые/новые файлы в root-relative ZIP без внешнего `DMXWB/`;
- при FAIL остаётся в текущем gate;
- после нового SHA снова читает актуальный репозиторий.

## 5. Формат шага

Обычный handoff:

1. краткий scope;
2. актуализация из GitHub;
3. готовые файлы в ZIP;
4. PowerShell-команда распаковки;
5. Linux/WB8 команды только если они нужны;
6. конкретные PASS/FAIL критерии;
7. Git-команды только после PASS.

### ZIP

ZIP содержит **финальные файлы**, не patch/diff. Пути идут от корня репозитория. Внешний каталог `DMXWB/` не добавляется.

Стандартная распаковка:

```powershell
Expand-Archive -Path "C:\Users\pereverworkki\Downloads\<PACKAGE>.zip" -DestinationPath "C:\Projects\DMXWB" -Force
```

### Build/test команды

C++ build/test команды выдаются для Local Linux/WSL и, при необходимости, для WB8 target workflow. Windows/MSVC build не требуется.

### Git после PASS

```powershell
Set-Location C:\Projects\DMXWB

git add .
git status --short
git commit -m "<meaningful commit message>"
git push

git rev-parse HEAD
```

Если пришла ошибка — commit не выполняется. Если пришёл новый SHA — текущий шаг считается внесённым и можно переходить дальше по roadmap.

## 6. Документация

### 6.1. Нормативные документы проекта

Документация должна соответствовать актуальному состоянию:

- `docs/TECHNICAL_SPEC.md` — продуктовые требования;
- `docs/ROADMAP.md` — gates и PASS criteria;
- `docs/PROJECT_STATE.md` — текущая оперативная точка;
- `README.md` — короткая точка входа;
- `AGENTS.md` — workflow/build policy и handoff rules.

Именно эти документы определяют текущее состояние и требования DMXWB.

### 6.2. Reusable engineering reference

`docs/reference/` содержит переносимую инженерную базу знаний, полученную в ходе проекта.

Reference-документы:

- объясняют реализацию, исследования, измерения, ограничения и неудачные подходы;
- могут использоваться в других проектах;
- обязаны явно разделять `Confirmed`, общие/portable принципы и DMXWB-specific решения;
- обязаны указывать проверенную hardware/software среду и внешние источники, если они существенны;
- **не заменяют** `TECHNICAL_SPEC.md`, `PROJECT_STATE.md` или `ROADMAP.md`.

Если reusable reference и нормативный проектный документ расходятся, для DMXWB приоритет имеет актуальный нормативный документ; reference затем должен быть обновлён.

Будущие крупные подсистемы допускается документировать отдельными reference/architecture документами после достижения соответствующего зрелого этапа, а не заранее как будто они уже реализованы.

Не создавать параллельные документы с дублирующими архитектурными решениями без необходимости.

## 7. Состояния утверждений

Использовать:

- **Confirmed** — доказано кодом, тестом или измерением;
- **Decided** — принято как требование/архитектурное решение;
- **Hypothesis** — рабочее предположение;
- **Deferred** — сознательно отложено.

Не выдавать Hypothesis за Confirmed.

## 8. Gate discipline

Если тест gate не прошёл:

- не переходить дальше;
- диагностировать причину в текущем gate;
- минимизировать scope исправления;
- повторить проверку.

Документационный/build-enablement step сам по себе не означает PASS engineering gate.

## 9. Приоритеты реализации

1. стабильный физический DMX output;
2. корректность source switching;
3. надёжность Art-Net и recovery;
4. корректная Fixture/Group/Scene model;
5. MQTT-интеграция WB;
6. static web;
7. offline deployment и diagnostics.

Web, MQTT callback, persistence и Art-Net parser не должны блокировать DMX output loop.

## 10. Hardware safety

При физических измерениях RS-485/DMX не давать инструкций по подключению earth-grounded oscilloscope ground к A/B без подтверждения прибора и безопасной схемы измерения.

## 11. Что не использовать как текущую архитектуру

Не возвращаться без явного изменения требований к:

- DMX engine внутри WBEC STM32;
- custom `wbec-uart`;
- MOD1 как DMX output;
- Docker.

Текущий путь — Linux C++ userspace application на WB8, физический DMX через встроенный RS-485 и штатная инфраструктура Wiren Board.
