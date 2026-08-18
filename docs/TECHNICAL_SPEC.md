# Техническое задание DMXWB

**Статус документа:** согласованная рабочая спецификация для последующей разработки  
**Дата:** 2026-08-18  
**Целевая платформа:** Wiren Board 8.5.1, Linux Debian/WB stable  
**Язык основного приложения:** C++20  
**Назначение документа:** единый источник требований к конечному приложению DMXWB.

---

## 1. Назначение системы

DMXWB — самостоятельное C++-приложение для Wiren Board, предназначенное для непрерывного управления одной физической линией DMX512 через встроенный RS-485 контроллера.

Приложение поддерживает два независимых источника данных:

1. **WB MQTT** — логическое управление RGBW-светильниками, группами и сценами.
2. **ART-NET** — прямое управление физическими DMX-каналами через сеть Ethernet.

В каждый момент времени физический DMX-выход использует только один выбранный источник.

Переключение источника выполняется явно через MQTT. Автоматического переключения между WB MQTT и ART-NET нет.

Веб-интерфейс DMXWB является локальным средством настройки, диагностики и ручного управления приложением. Он не является системой диспетчеризации. Веб-интерфейс полностью статический и взаимодействует с C++-приложением исключительно через MQTT.

---

## 2. Архитектура верхнего уровня

```text
                           Browser
                      /var/www/dmxwb
                             |
                      MQTT WebSocket
                           /mqtt
                             |
                             v
                         Mosquitto
                             |
                             v
┌──────────────────────────────────────────────────────┐
│                     DMXWB C++20                      │
│                                                      │
│   MQTT input                 Art-Net input           │
│       |                           |                  │
│       v                           v                  │
│  Fixture/Group/Scene         Art-Net state           │
│       |                           |                  │
│       v                           v                  │
│  mqtt DMX snapshot          artnet snapshot          │
│       |                           |                  │
│       └──────────────┬────────────┘                  │
│                      v                               │
│               Source selector                       │
│              MQTT / ART-NET                         │
│                      |                               │
│                      v                               │
│               DMX Output Thread                     │
└──────────────────────┬───────────────────────────────┘
                       |
                /dev/ttyRS485-1
                       |
                     DMX512
```

Основные архитектурные инварианты:

- один процесс `dmxwb`;
- один физический DMX-выход;
- только модуль `DmxOutput` владеет serial-портом;
- MQTT и Art-Net никогда непосредственно не пишут в serial-порт;
- MQTT и Art-Net работают параллельно независимо от выбранного Source;
- DMX-поток формируется отдельным рабочим потоком;
- логические изменения передаются в DMX-поток только целыми snapshot-ами;
- веб не имеет отдельного REST/HTTP API;
- C++ backend является источником истины для конфигурации и состояния.

---

## 3. Источник управления

Поддерживаются два значения:

```text
mqtt
artnet
```

Отображаемые названия:

```text
WB MQTT
ART-NET
```

### 3.1. WB MQTT

В режиме WB MQTT приложение:

- хранит состояние светильников;
- применяет команды отдельных светильников;
- применяет групповые команды;
- применяет сцены;
- рассчитывает четыре физических канала RGBW;
- формирует `mqtt_buffer`.

### 3.2. ART-NET

В режиме ART-NET приложение:

- принимает `ArtDmx`;
- работает непосредственно с DMX-каналами;
- не применяет модель Fixture;
- не применяет Group;
- не применяет Scene;
- не применяет Brightness/Temperature/Color-алгоритмы;
- формирует `artnet_buffer`.

### 3.3. Независимость источников

Неактивный источник продолжает работать.

Если выбран ART-NET:

- MQTT-команды продолжают менять логическое состояние Fixture/Group/Scene;
- `mqtt_buffer` остаётся актуальным;
- физический DMX использует Art-Net.

Если выбран WB MQTT:

- Art-Net receiver продолжает принимать пакеты;
- сохраняет текущее Art-Net состояние;
- отслеживает связь с пультом;
- отвечает на discovery-пакеты;
- физический DMX использует MQTT.

### 3.4. Переключение

Переключение выполняется только на границе DMX-кадров.

Один уже начатый физический кадр никогда не должен содержать данные одновременно из MQTT и Art-Net.

При `ART-NET -> WB MQTT` следующий кадр целиком берётся из текущего `mqtt_buffer`.

При `WB MQTT -> ART-NET`:

- если корректный Art-Net snapshot уже существует — следующий кадр берётся из него;
- если после запуска процесса ещё не было ни одного корректного ArtDmx — текущий физический выход не заменяется искусственным нулевым кадром; система ожидает первый корректный ArtDmx.

### 3.5. Сохранение Source

Последний выбранный Source сохраняется.

После перезапуска:

```text
mqtt   -> mqtt
artnet -> artnet
```

Ни наличие Art-Net, ни его потеря не изменяют Source автоматически.

---

## 4. Физический DMX-выход

### 4.1. Порт

Поддерживаются встроенные порты Wiren Board:

```text
/dev/ttyRS485-1
/dev/ttyRS485-2
```

Значение по умолчанию:

```text
/dev/ttyRS485-1
```

В каждый момент приложение использует только один порт.

### 4.2. Формат

Физическая передача:

```text
250000 baud
8 data bits
No parity
2 stop bits
Start Code = 0x00
```

Кадр:

```text
BREAK
Start Code 0x00
DMX channel 1
DMX channel 2
...
DMX channel N
```

### 4.3. BREAK

Базовый способ генерации BREAK должен воспроизводить проверенный подход, опубликованный Wiren Board для встроенного RS-485:

```text
переключение serial на 38400 baud
передача 0x00
flush
возврат на 250000 8N2
передача Start Code + data
```

Конкретная реализация C++ должна быть проверена на реальном WB и, при наличии измерительного оборудования, осциллографом/логическим анализатором.

Программный транспорт не должен изменяться на kernel/WBEC-реализацию без отдельного пересмотра ТЗ.

### 4.4. Непрерывность передачи

DMX передаётся непрерывно независимо от того, меняются значения или нет.

MQTT/Art-Net обновляют только внутренние данные. Физический DMX-цикл работает самостоятельно.

### 4.5. Длина кадра

Постоянная передача всех 512 каналов не требуется.

Количество физических DMX slots определяется последним используемым адресом конфигурации светильников:

```text
dmx_slot_count = highest configured fixture address
```

Пример:

```text
Count = 10
Start Address = 1
-> Fixtures = 1..40
-> передаются channels 1..40
```

Пример:

```text
Count = 10
Start Address = 21
-> Fixtures = 21..60
-> физический кадр содержит channels 1..60
```

В WB MQTT неиспользуемые каналы внутри `1..N` равны нулю.

В ART-NET все каналы `1..N` являются прямыми Art-Net каналами, даже если часть из них не занята объектами Fixture.

При `Fixture Count = 0` пользовательских DMX slots нет; приложение не должно генерировать выдуманный набор каналов.

### 4.6. Refresh Rate

Пользовательский параметр:

```text
DMX Refresh Rate
```

Диапазон интерфейса:

```text
minimum = 10 Hz
maximum = 44 Hz
default = 30 Hz
```

Частота меняется без закрытия serial-порта.

Период задаётся между началами кадров, а не как `sleep()` после завершения кадра:

```text
T0
T0 + period
T0 + 2 * period
...
```

Приложение не должно молча принимать частоту, которую физически невозможно выдержать при текущем `dmx_slot_count`.

Фактически допустимый максимум:

```text
min(44 Hz, максимум для текущей длины кадра и измеренного transport overhead)
```

Если выбранное значение невозможно обеспечить, конфигурация отклоняется с понятной ошибкой.

### 4.7. Ошибка serial

Ошибка открытия или записи в RS-485 не должна завершать процесс.

Алгоритм:

```text
error
-> закрыть fd
-> отметить DMX Output Error
-> периодически повторять открытие
-> восстановить настройки serial
-> продолжить с актуального snapshot
```

MQTT и Art-Net продолжают работать во время ошибки DMX.

### 4.8. Stop/restart

Штатная остановка или restart приложения не являются командой Blackout.

Специальный нулевой кадр перед завершением не отправляется.

---

## 5. Модель RGBW-светильника

Каждый светильник занимает четыре последовательных DMX-адреса:

```text
Address + 0 = R
Address + 1 = G
Address + 2 = B
Address + 3 = W
```

Порядок каналов фиксирован: RGBW.

### 5.1. Внутреннее сохранённое состояние

Для каждого Fixture хранятся:

```text
requested_power : bool
red             : 0..255
green           : 0..255
blue            : 0..255
white           : 0..255
brightness      : 0..100
temperature     : 0..100
```

`temperature` хранит последнюю уставку шкалы температуры.

### 5.2. Интерфейсы управления

У Fixture есть:

```text
Power
Red
Green
Blue
Color
Brightness
Temperature
Reset
Name
```

Разные интерфейсы могут перехватывать управление четырьмя каналами.

### 5.3. Индивидуальное RGB

Команда `Red`, `Green` или `Blue`:

- изменяет только указанный RGB-канал;
- сохраняет остальные два RGB-канала;
- немедленно устанавливает `W = 0`.

Пример:

```text
до:
R=255 G=255 B=255 W=180

Red/on = 100

после:
R=100 G=255 B=255 W=0
```

### 5.4. Color

`Color` задаёт сразу RGB:

```text
R/G/B = выбранный цвет
W = 0
```

Color и отдельные R/G/B являются двумя интерфейсами изменения одного RGB-состояния.

### 5.5. Temperature

Шкала:

```text
0%   = холодный
100% = тёплый
```

При любой команде Temperature:

```text
R = 255
G = 255
B = 255
W = round(Temperature * 255 / 100)
```

Примеры:

```text
0%   -> 255/255/255/0
50%  -> 255/255/255/~128
100% -> 255/255/255/255
```

Этот алгоритм предназначен для используемых светильников, у которых W-канал фактически добавляет жёлтый/тёплый свет.

### 5.6. Brightness

Brightness не разрушает сохранённое `R/G/B/W`.

Итоговые значения:

```text
scaled_channel = saved_channel * brightness / 100
```

Операция применяется ко всем четырём каналам.

### 5.7. Power

`Power OFF`:

- устанавливает `requested_power = false`;
- не изменяет сохранённые R/G/B/W;
- не изменяет Brightness;
- фактический физический выход становится `0/0/0/0`.

`Power ON`:

- устанавливает `requested_power = true`;
- восстанавливает сохранённое состояние;
- применяет текущий Brightness.

### 5.8. Reset

Кнопка **«Сбросить настройки»** выполняет:

```text
requested_power = ON
Brightness      = 100
R               = 255
G               = 255
B               = 255
W               = 255
```

После Reset физически все четыре канала должны получить максимум.

### 5.9. Фактический статус

Фактическое состояние Fixture считается включённым, если после Brightness и Power хотя бы один из четырёх каналов больше нуля:

```text
actual_on =
    actual_r > 0 ||
    actual_g > 0 ||
    actual_b > 0 ||
    actual_w > 0
```

Например:

```text
requested_power = ON
brightness = 0
-> actual RGBW = 0/0/0/0
-> фактический Power status = OFF
```

### 5.10. Фактический RGB

Публикуемый RGB status должен отображать реальные значения после Brightness и Power:

```text
actual_r
actual_g
actual_b
```

При Power OFF:

```text
Red   = 0
Green = 0
Blue  = 0
Color = 0;0;0
```

Сохранённые значения при этом продолжают существовать во внутреннем состоянии и восстанавливаются при включении.

---

## 6. Конструктор светильников

### 6.1. Параметры

Пользователь задаёт:

```text
Fixture Count
Start Address
```

Каждый Fixture занимает ровно 4 адреса.

Формула физического адреса:

```text
fixture_start = start_address + fixture_index * 4
```

### 6.2. Валидация

При `count > 0`:

```text
start_address >= 1
start_address + count * 4 - 1 <= 512
```

Максимальное количество рассчитывается автоматически.

`Fixture Count = 0` разрешён.

### 6.3. Stable ID

Каждый Fixture получает постоянный технический ID.

ID:

- не зависит от DMX-адреса;
- не зависит от отображаемого имени;
- не меняется при изменении Start Address;
- не переиспользуется после удаления.

Пример:

```text
fixture_12
```

При уменьшении Count удаляются последние Fixture в текущем порядке. При последующем увеличении создаются новые ID, а не возвращаются ID удалённых объектов.

Это предотвращает случайное применение старой Scene к новому физическому светильнику.

### 6.4. Начальное состояние нового Fixture

Новый Fixture создаётся:

```text
requested_power = OFF
R = 255
G = 255
B = 255
W = 255
Brightness = 100
Temperature = 100
```

Он не загорается автоматически, но после Power ON готов к полному включению.

### 6.5. Изменение Start Address

Изменение Start Address:

- не меняет Fixture ID;
- не меняет Name;
- не меняет сохранённый RGBW;
- не меняет Brightness;
- не меняет Power;
- не меняет Group membership;
- меняет только физическое DMX-сопоставление.

Новый `mqtt_buffer` строится полностью до атомарной публикации snapshot.

### 6.6. Name

Каждый Fixture имеет изменяемое текстовое название.

По умолчанию:

```text
Светильник 1
Светильник 2
...
```

Name не обязано быть уникальным.

---

## 7. Группы

Group не имеет собственного DMX-адреса.

Она содержит:

```text
stable ID
Name
Members[]
```

Members ссылаются только на stable Fixture ID.

Один Fixture может одновременно входить в несколько групп.

### 7.1. Команды группы

Группа предоставляет те же управляющие интерфейсы, что Fixture:

```text
Power
Red
Green
Blue
Color
Brightness
Temperature
Reset
```

Команда Group реально изменяет состояние каждого входящего Fixture.

Отдельного группового слоя mixing нет.

Последняя применённая команда определяет состояние конкретного Fixture.

### 7.2. Power группы

`Group Power OFF` выключает каждого участника, сохраняя его собственные R/G/B/W и Brightness.

`Group Power ON` включает каждого участника в его собственном сохранённом состоянии.

Групповое включение не делает все светильники одинаковыми.

### 7.3. Reset группы

Group Reset применяет Fixture Reset ко всем участникам.

### 7.4. Group Status

Group считается включённой, если хотя бы один её Fixture фактически включён:

```text
group_on = OR(member.actual_on)
```

Если группа пуста:

```text
Power status = OFF
```

### 7.5. Остальные состояния группы

Групповые R/G/B/Color/Brightness/Temperature являются последними уставками, отправленными через эту группу.

Они не пересчитываются при последующем индивидуальном изменении одного из участников.

Единственный агрегированный фактический статус группы — Power ON/OFF.

### 7.6. Удаление Fixture

При удалении Fixture его ID автоматически удаляется из всех Group.

Пустая Group остаётся существовать.

### 7.7. Удаление Group

Удаление Group не меняет состояния её бывших участников.

### 7.8. ID и Name

Group ID:

- постоянный;
- не переиспользуется после удаления.

Name:

- изменяемый;
- текстовый;
- не обязан быть уникальным.

---

## 8. Сцены

Scene — сохранённый snapshot индивидуального состояния светильников.

Она не является Group и не является отдельным DMX-слоем.

### 8.1. Snapshot

Для каждого Fixture, присутствовавшего при сохранении:

```text
fixture_id
R
G
B
W
Brightness
requested_power
```

Scene не зависит от DMX-адресов.

Scene не хранит историю способа управления. Не важно, было состояние получено через Color, Temperature или отдельные каналы.

### 8.2. Операции

Поддерживаются:

```text
Create from current state
Apply
Overwrite from current state
Rename
Delete
```

### 8.3. Apply

При Apply:

1. backend изменяет все соответствующие Fixture в памяти;
2. строит новый `mqtt_buffer`;
3. атомарно публикует новый DMX snapshot;
4. затем публикует MQTT states.

Все приборы должны перейти в сцену одним логическим DMX-обновлением, без последовательного визуального перебора.

### 8.4. Изменение количества Fixture

Если после сохранения сцены появились новые Fixture:

- Scene их не изменяет.

Если Fixture из snapshot больше не существует:

- его запись игнорируется.

Удалённые Fixture ID не переиспользуются.

### 8.5. Source

Apply Scene не переключает Source.

При выбранном ART-NET сцена изменяет MQTT-состояние и `mqtt_buffer`, но физический выход остаётся Art-Net.

### 8.6. ID и Name

Scene ID:

- постоянный;
- не переиспользуется.

Name:

- изменяемый;
- текстовый;
- используется будущими интерфейсами для отображения кнопок сцен.

---

## 9. MQTT: общие правила

MQTT broker на целевом Wiren Board:

```text
127.0.0.1:1883
```

C++ использует `libmosquitto`.

MQTT не является базой конфигурации.

Источник истины:

```text
disk -> C++ model -> MQTT
```

а не:

```text
retained MQTT -> восстановление приложения
```

### 9.1. State и Command

Общее правило:

```text
state:
/.../controls/<control>

command:
/.../controls/<control>/on
```

Команды `/on`:

- всегда `retain=false`;
- retained-команды должны игнорироваться.

State и metadata:

- публикуются retained;
- при reconnect выполняется полная републикация актуального состояния.

### 9.2. Имена технических controls

Технические topic IDs используют lowercase/snake_case.

Человеко-читаемые русские/английские названия задаются через metadata.

---

## 10. MQTT-устройство DMXWB в штатном Wiren Board

В стандартном web Wiren Board должны отображаться только:

```text
Status
Source
```

Топики:

```text
/devices/dmxwb/controls/status
/devices/dmxwb/controls/source
/devices/dmxwb/controls/source/on
```

### 10.1. Status

Readonly text/enum:

```text
running
error
off
```

Отображение:

```text
Работает
Ошибка
Выключен
```

`off` используется как MQTT Last Will и публикуется при штатном shutdown перед disconnect.

### 10.2. Source

Writable text/enum:

```text
mqtt
artnet
```

Отображение:

```text
WB MQTT
ART-NET
```

Source state retained.

---

## 11. MQTT Fixture

Техническое устройство:

```text
/devices/dmxwb_fixture_<id>/
```

Controls:

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

Пример:

```text
/devices/dmxwb_fixture_12/controls/power
/devices/dmxwb_fixture_12/controls/power/on
```

### 11.1. Типы

| Control | Type | Диапазон/формат |
|---|---|---|
| `name` | text | UTF-8 text |
| `power` | switch | `0` / `1` |
| `red` | range | `0..255` |
| `green` | range | `0..255` |
| `blue` | range | `0..255` |
| `color` | rgb | `R;G;B` |
| `brightness` | range | `0..100 %` |
| `temperature` | range | `0..100 %` |
| `reset` | pushbutton | command `1` |

### 11.2. Семантика state

`power` — фактический ON/OFF по итоговым каналам.

`red/green/blue/color` — фактические физические значения после Brightness и Power.

`brightness` — текущая уставка Brightness.

`temperature` — последняя уставка Temperature.

`name` — сохранённое имя.

`reset` не имеет полезного постоянного factual state; это stateless pushbutton.

### 11.3. Скрытие из стандартного WB web

Fixture MQTT controls предназначены для MQTT-интеграции и собственного web DMXWB, но не должны засорять штатную страницу Wiren Board.

Metadata Fixture controls публикуются с:

```json
"hidden": true
```

Acceptance test должен подтвердить, что стандартный WB UI показывает только системное устройство `dmxwb`, а Fixture/Group/Scene остаются доступны через MQTT.

---

## 12. MQTT Group

Устройство:

```text
/devices/dmxwb_group_<id>/
```

Controls:

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

Типы совпадают с Fixture.

`power` state является вычисленным агрегированным статусом:

```text
1, если хотя бы один member фактически ON
0, если все OFF
```

Остальные управляющие state группы отражают последнюю команду, отправленную через Group.

Все controls скрыты из штатного WB web через metadata `hidden=true`.

---

## 13. MQTT Scene

Устройство:

```text
/devices/dmxwb_scene_<id>/
```

Controls:

```text
name
apply
```

Типы:

```text
name  = text
apply = pushbutton
```

Команда Apply:

```text
/devices/dmxwb_scene_<id>/controls/apply/on
payload = 1
retain = false
```

Controls скрыты из штатного WB web.

---

## 14. MQTT API собственного web

Веб DMXWB не редактирует файлы и не вызывает systemd напрямую.

Для структуры приложения используется отдельный MQTT namespace.

### 14.1. Каноническая конфигурация

Retained:

```text
/dmxwb/config
```

Payload содержит полную каноническую конфигурацию и поле:

```text
revision
```

### 14.2. Изменение конфигурации

Non-retained:

```text
/dmxwb/config/set
```

Web отправляет:

```text
request_id
expected_revision
полную предлагаемую конфигурацию
```

Backend:

1. полностью парсит payload;
2. проверяет schema/version;
3. проверяет все ссылки и диапазоны;
4. проверяет DMX addresses;
5. проверяет Refresh Rate;
6. строит новую внутреннюю конфигурацию;
7. атомарно записывает `config.json`;
8. увеличивает revision;
9. одним действием применяет новую конфигурацию;
10. публикует новый retained `/dmxwb/config`;
11. публикует result.

Если `expected_revision` устарел, save отклоняется. Это предотвращает перезапись изменений двумя открытыми вкладками.

### 14.3. Результат конфигурации

Non-retained:

```text
/dmxwb/config/result
```

Минимально:

```text
request_id
ok
revision
error_code
message
```

Ошибка новой конфигурации никогда не разрушает уже работающую старую конфигурацию.

### 14.4. Runtime state для собственного web

Retained:

```text
/dmxwb/state
```

Этот state предназначен именно для собственного web и содержит сохранённое логическое состояние:

```text
source

Fixture:
    id
    requested_power
    R
    G
    B
    W
    Brightness
    Temperature
```

Он нужен, в частности, чтобы web мог помнить сохранённый цвет выключенного светильника, даже когда factual `/devices/.../red` равен нулю.

`/devices/...` остаётся factual состоянием, а `/dmxwb/state` — каноническим внутренним логическим snapshot для интерфейса управления.

### 14.5. Диагностика

Retained:

```text
/dmxwb/status
```

Содержит:

```text
application
dmx
mqtt
artnet
configuration
last_error
```

### 14.6. Scene lifecycle

Создание и перезапись Scene выполняются на backend из текущего внутреннего Fixture state.

Команды:

```text
/dmxwb/scenes/create
/dmxwb/scenes/<scene_id>/overwrite
/dmxwb/scenes/<scene_id>/delete
```

Все команды:

```text
retain=false
```

и содержат `request_id`.

Rename Scene может выполняться либо через `/name/on`, либо как часть сохранения конфигурации; backend в любом случае является источником истины.

### 14.7. Live controls

Обычные команды света собственного web идут через те же `/devices/.../controls/.../on`, что и любые внешние MQTT-клиенты.

Таким образом не существует отдельного второго механизма управления светом.

---

## 15. Web UI

### 15.1. Общие требования

Путь:

```text
/var/www/dmxwb
```

URL:

```text
http://<WB-address>/dmxwb/
```

Структура:

```text
www/dmxwb/
    index.html
    app.js
    model.js
    mqtt-client.js
    styles.css
```

Требования:

- HTML/CSS/vanilla JavaScript;
- нет Node.js runtime;
- нет npm/build step;
- нет React/Vue;
- нет внешних интернет-зависимостей;
- MQTT WebSocket `/mqtt`;
- hostname определяется из текущего URL;
- reconnect полностью автоматический.

Архитектура должна использовать проверенный подход MDVWB: статический web + собственный MQTT WebSocket client + Mosquitto как единственный runtime IPC.

### 15.2. Разделы

Web содержит:

```text
Управление
Светильники и группы
Сцены
Настройки
```

### 15.3. Fixture card

Функции:

```text
Name
Power
Color picker
R slider 0..255
G slider 0..255
B slider 0..255
Brightness 0..100 %
Temperature 0..100 %
Reset
```

У каждого slider рядом отображается числовое значение.

Temperature визуально:

```text
0% Холодный ---------------- 100% Тёплый
```

### 15.4. Slider publishing

Во время движения slider команды передаются плавно.

Web ограничивает частоту публикации одного control примерно до 20–30 команд/с и обязательно отправляет последнее значение.

### 15.5. Factual confirmation

Web не должен считать команду выполненной только потому, что MQTT publish прошёл.

После `/on` интерфейс получает подтверждённое состояние через factual base topic и `/dmxwb/state`.

### 15.6. Структурные настройки

Live controls применяются сразу.

Структурные настройки применяются только после явного:

```text
Применить
```

К структурным относятся:

```text
DMX Port
Refresh Rate
Fixture Count
Start Address
Group membership
Art-Net Universe
```

До Apply web работает с локальным draft.

### 15.7. Reconnect браузера

При потере MQTT:

- показать состояние «Нет связи с DMXWB»;
- заблокировать отправку новых команд;
- автоматически переподключаться;
- после reconnect повторно подписаться;
- получить retained config/state/status;
- не переотправлять старые команды.

Reload страницы не должен требоваться.

---

## 16. Art-Net

### 16.1. Общие параметры

Протокол:

```text
Art-Net 4
UDP
port 6454 (0x1936)
IPv4
```

Поддерживается один физический Art-Net Port-Address/universe.

Пользовательская настройка:

```text
Art-Net Universe
```

Допустимый диапазон приложения:

```text
0..32767
default = 0
```

Примечание: актуальный Art-Net 4 считает Port-Address 0 deprecated; значение 0 сохраняется в DMXWB как режим совместимости с распространёнными контроллерами, использующими нумерацию с нуля.

### 16.2. Пакеты

Минимально обязательны:

```text
ArtDmx
ArtPoll
ArtPollReply
```

`ArtPollReply` должен корректно объявлять DMXWB как один DMX output node, подписанный на настроенный Port-Address.

### 16.3. ArtDmx validation

Принимаются только пакеты:

- с корректной сигнатурой `Art-Net\0`;
- корректным OpCode;
- поддерживаемой версией протокола;
- нужным Port-Address;
- корректной длиной;
- корректным размером UDP payload.

Для ArtDmx `Length` должен быть чётным числом `2..512`.

Некорректный пакет игнорируется без нарушения текущего выхода.

### 16.4. Переменная Length

Art-Net хранит persistent `artnet_state[512]`.

Если:

```text
Length < dmx_slot_count
```

обновляются только каналы:

```text
1..Length
```

Остальные сохраняют предыдущее значение.

Если:

```text
Length > dmx_slot_count
```

используются только:

```text
1..dmx_slot_count
```

Лишние данные игнорируются для физического DMX.

Пример при `dmx_slot_count = 40`:

```text
Length=10
-> обновить 1..10
-> 11..40 Hold Last

Length=100
-> использовать только 1..40
```

При увеличении `dmx_slot_count` новые ранее отсутствовавшие каналы Art-Net инициализируются нулём до первого обновления.

### 16.5. Output cadence

Art-Net packet arrival не запускает физическую serial-передачу непосредственно.

Art-Net только обновляет snapshot.

DMX Output передаёт последний snapshot со своим настроенным Refresh Rate.

Таким образом даже если Art-Net приходит чаще, физический DMX не превышает допустимую частоту.

### 16.6. Hold Last

При прекращении ArtDmx:

- `artnet_buffer` не очищается;
- физический DMX продолжает повторять последний корректный кадр;
- Source не переключается;
- restart не требуется.

### 16.7. Состояния Art-Net

Внутренний автомат:

```text
WAITING
ACTIVE
LOST
CONFLICT
```

`WAITING` — после запуска ещё нет корректного источника.

`ACTIVE` — корректные ArtDmx принимаются.

`LOST` — активный источник перестал передавать дольше timeout.

`CONFLICT` — обнаружен второй источник того же Port-Address, пока основной ещё активен.

### 16.8. Timeout

Базовый timeout:

```text
3 seconds
```

Он используется только для диагностики/освобождения active-source lock.

Timeout не вызывает:

- Blackout;
- очистку buffer;
- переключение на MQTT;
- restart.

### 16.9. Sequence

Если `Sequence = 0`:

- sequence checking отключён.

Если `Sequence != 0`:

- пакеты контролируются на порядок;
- устаревший/запоздавший пакет не должен перезаписывать более новое состояние;
- rollover `0xFF -> 0x01` учитывается.

При переходе в LOST и при выборе нового источника sequence tracking сбрасывается.

Это позволяет нормально восстановиться после перезагрузки пульта, если его sequence начинается заново.

### 16.10. Несколько источников

HTP/LTP mixing в DMXWB не выполняется.

Art-Net допускает считать несколько источников конфликтом; DMXWB использует этот вариант.

Алгоритм:

```text
нет active source
-> первый корректный ArtDmx становится active

active source существует
-> его пакеты принимаются

тот же Port-Address с другого IP
-> второй источник не перехватывает выход
-> состояние CONFLICT
-> данные второго источника игнорируются

active source потерян >= timeout
-> lock освобождается
-> следующий корректный источник может стать active
```

Это предотвращает случайный перехват света вторым пультом и одновременно обеспечивает автоматическое восстановление.

### 16.11. Восстановление связи

Art-Net subsystem должен самостоятельно восстанавливаться после:

- кратковременного обрыва Ethernet;
- долгого обрыва;
- выдёргивания/возврата кабеля;
- перезагрузки пульта;
- выключения/включения пульта;
- изменения IP пульта;
- down/up сетевого интерфейса WB;
- временной ошибки UDP socket.

Ни один из этих случаев не должен требовать restart DMXWB.

Socket при необходимости пересоздаётся и повторно bind-ится на UDP 6454.

---

## 17. Внутренняя C++ архитектура

### 17.1. Процесс

Один executable:

```text
dmxwb
```

Один systemd service:

```text
dmxwb.service
```

### 17.2. Логические потоки

```text
Main
MQTT
Controller
Art-Net
DMX Output
Persistence
```

Реальная реализация может объединять Main/Persistence там, где это не нарушает инварианты, но DMX Output остаётся отдельным рабочим контекстом.

### 17.3. MQTT callback

Callback:

1. проверяет topic;
2. парсит payload;
3. валидирует базовый формат;
4. формирует Command;
5. помещает Command в thread-safe queue.

Callback не изменяет serial и не должен выполнять длительные файловые операции.

### 17.4. Controller

Controller является единственным владельцем логической модели:

```text
Fixtures
Groups
Scenes
Source
```

Команды выполняются последовательно.

### 17.5. DMX snapshots

Controller не даёт DMX thread прямую mutable-ссылку на Fixture model.

После изменения он строит полностью новый:

```text
DmxSnapshot
```

Snapshot содержит:

```text
channels[]
slot_count
generation/revision
```

Передача snapshot выполняется double-buffer/immutable-snapshot способом.

### 17.6. Art-Net snapshot

Art-Net thread хранит persistent `artnet_state` и после корректного пакета публикует целый новый snapshot.

### 17.7. Scene/Group atomicity

Group/Scene операция сначала целиком меняет внутреннюю модель, затем публикует один новый DMX snapshot.

DMX thread не должен видеть промежуточное состояние операции.

### 17.8. Приоритет DMX над MQTT publication

После логической команды порядок:

```text
1. обновить внутреннюю модель
2. построить и опубликовать DMX snapshot
3. поставить MQTT factual publications
```

Физический свет не ждёт публикации десятков MQTT state сообщений.

---

## 18. Конфигурация и persistence

### 18.1. Файлы

Конфигурация:

```text
/etc/dmxwb/config.json
```

Runtime state:

```text
/var/lib/dmxwb/state.json
```

### 18.2. config.json

Содержит:

```text
version
revision

DMX:
    port
    refresh_rate

Art-Net:
    universe

Fixtures:
    ordered fixture records
        id
        name
    count
    start_address

Groups:
    id
    name
    member fixture IDs

Scenes:
    id
    name
    fixture snapshots

ID counters:
    next_fixture_id
    next_group_id
    next_scene_id
```

### 18.3. state.json

Содержит:

```text
version

source

Fixtures:
    id
    requested_power
    R
    G
    B
    W
    brightness
    temperature
```

Art-Net transient buffer не является канонической конфигурацией и не используется для восстановления логической MQTT-модели.

### 18.4. Асинхронная запись state

DMX реагирует в RAM немедленно.

После изменения runtime state:

```text
dirty = true
```

Запись:

```text
через 2 s после последнего изменения
ИЛИ
не реже одного раза в 10 s при непрерывных изменениях
```

Эти интервалы являются внутренними константами, не пользовательской настройкой.

### 18.5. Atomic file replace

Запись config/state:

```text
1. serialize in memory
2. write *.tmp
3. flush/fsync
4. atomic rename
```

Старый корректный файл не уничтожается до успешного завершения нового.

### 18.6. Shutdown

При SIGTERM:

- прекратить приём новых команд;
- немедленно сохранить dirty state;
- остановить workers;
- закрыть serial;
- закрыть UDP;
- корректно disconnect MQTT;
- завершить процесс.

### 18.7. Ошибка state.json

Если `state.json` повреждён:

- `config.json` остаётся рабочим;
- приложение создаёт безопасные default Fixture states;
- сообщает ошибку;
- продолжает работу.

### 18.8. Ошибка config.json

Если config повреждён:

- повреждённый файл не перезаписывается автоматически;
- приложение запускает безопасную минимальную конфигурацию в памяти;
- публикует Configuration Error;
- пользователь может сохранить новую корректную конфигурацию через web.

Минимальные defaults:

```text
DMX Port = /dev/ttyRS485-1
Refresh = 30 Hz
Art-Net Universe = 0
Fixture Count = 0
Start Address = 1
```

---

## 19. systemd и жизненный цикл

Service:

```text
Type=simple
Restart=on-failure
RestartSec=2s
```

Сервис не должен требовать наличия Mosquitto как условия запуска DMX-процесса.

Временные неисправности восстанавливаются внутри приложения:

```text
MQTT lost    -> reconnect
Art-Net lost -> Hold Last + auto recovery
RS-485 lost  -> reopen/retry
network lost -> auto recovery
```

systemd restart применяется только при реальном завершении/аварии самого процесса.

### 19.1. MQTT reconnect

После reconnect:

1. восстановить subscriptions;
2. опубликовать metadata;
3. опубликовать `/devices/dmxwb`;
4. опубликовать Fixture/Group/Scene factual states;
5. опубликовать `/dmxwb/config`;
6. опубликовать `/dmxwb/state`;
7. опубликовать `/dmxwb/status`.

Никакая команда из retained MQTT не выполняется.

---

## 20. Ошибки и диагностика

В собственном web используются уровни:

```text
OK
WARNING
ERROR
```

Подсистемы:

```text
Application
DMX Output
MQTT
Art-Net
Configuration
```

### 20.1. Общий Status

`DMX Output Error` всегда означает общий `error`.

Если Source = ART-NET и Art-Net LOST:

```text
Status = error
```

Если Source = WB MQTT и Art-Net LOST, но MQTT и DMX работают:

```text
Status = running
Art-Net = warning
```

Неактивная неисправная подсистема не обязана переводить приложение в общий Error, если выбранный путь управления работает.

### 20.2. DMX diagnostics

Минимально:

```text
state
port
slot_count
configured_refresh
actual_refresh
frames_sent
last_error
recovery_state
```

### 20.3. Art-Net diagnostics

Минимально:

```text
state
universe
active_source_ip
last_packet_age
last_sequence
conflicting_source_ip
output_mode = Hold Last / Live
recovery_state
```

### 20.4. MQTT diagnostics

```text
Connected / Disconnected
Reconnecting
```

### 20.5. Config errors

Некорректная новая конфигурация:

- отклоняется целиком;
- старая конфигурация продолжает работать;
- web получает понятное сообщение;
- никакого частичного применения нет.

### 20.6. Логи

Приложение пишет stdout/stderr.

Основной журнал:

```text
journalctl -u dmxwb
```

Не логируются:

- каждый DMX frame;
- каждое промежуточное движение slider.

Логируются существенные события:

- startup/shutdown;
- config load/save;
- config reject;
- serial open/lost/recovered;
- MQTT connect/lost/recovered;
- Art-Net source found/lost/recovered/conflict;
- Scene apply;
- fatal errors.

---

## 21. Структура проекта

Рекомендуемая структура:

```text
DMXWB/
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
├── AGENTS.md
│
├── src/
│   ├── main.cpp
│   │
│   ├── controller/
│   │   ├── controller.cpp
│   │   └── controller.h
│   │
│   ├── model/
│   │   ├── fixture.cpp
│   │   ├── group.cpp
│   │   ├── scene.cpp
│   │   └── ...
│   │
│   ├── dmx/
│   │   ├── dmx_output.cpp
│   │   ├── dmx_transport.cpp
│   │   └── ...
│   │
│   ├── artnet/
│   │   ├── artnet_receiver.cpp
│   │   ├── artnet_protocol.cpp
│   │   └── ...
│   │
│   ├── mqtt/
│   │   ├── mqtt_client.cpp
│   │   ├── mqtt_contract.cpp
│   │   ├── mqtt_metadata.cpp
│   │   └── ...
│   │
│   ├── config/
│   │   ├── config.cpp
│   │   └── validation.cpp
│   │
│   └── persistence/
│       ├── state_store.cpp
│       └── atomic_file.cpp
│
├── www/
│   └── dmxwb/
│       ├── index.html
│       ├── app.js
│       ├── model.js
│       ├── mqtt-client.js
│       └── styles.css
│
├── deploy/
│   ├── dmxwb.service
│   ├── install_wirenboard.sh
│   └── config.example.json
│
├── tests/
│   ├── unit/
│   └── integration/
│
└── docs/
```

---

## 22. Зависимости

Обязательные:

```text
C++20
CMake
Threads
libmosquitto
POSIX serial/termios
POSIX UDP sockets
systemd на целевом WB
```

JSON-реализация должна быть компактной и пригодной для offline ARM64 deployment. Не допускается зависимость runtime от Node.js.

Production-сборка должна завершаться ошибкой, если отсутствует обязательная MQTT-зависимость, а не создавать частично работоспособный бинарник.

---

## 23. Тесты

### 23.1. Unit

Обязательно:

```text
RGB command -> W=0
Color -> RGB + W=0
Temperature calculation
Brightness scaling
Power restore
Reset
actual Power status
actual RGB state
DMX address calculation
dynamic slot_count
Refresh validation
Fixture config validation
stable IDs
Group operations
Group power aggregation
Scene snapshot/apply
Scene with missing/new Fixtures
ArtDmx parser
ArtDmx length validation
short ArtDmx Hold Last
ArtDmx truncation
Sequence/rollover
active source timeout
source conflict
Source selector
config parse/serialize
state parse/serialize
revision conflict
atomic update behavior
```

### 23.2. MQTT integration

Проверяется:

```text
/on command
-> C++ state
-> DMX snapshot
-> factual base state
```

Retained states доступны новому клиенту.

Retained `/on` игнорируется.

Удаление Fixture/Group/Scene очищает устаревшие retained MQTT topics.

### 23.3. Web

Проверяется:

- загрузка без интернет-доступа;
- работа без Node.js runtime;
- MQTT reconnect;
- Fixture controls;
- Color/RGB sync;
- Brightness;
- Temperature;
- Power restore;
- Reset;
- Fixture constructor;
- Group editor;
- Scene editor;
- Apply config;
- отображение backend validation errors;
- восстановление UI после reload/reconnect.

---

## 24. Hardware acceptance

### 24.1. DMX

На реальном Wiren Board и реальном RGBW-светильнике:

```text
R only
G only
B only
W only
all 0
all 255
Temperature 0/50/100
Brightness
Power OFF/ON restore
Reset
```

### 24.2. Частота

Проверить:

```text
10 Hz
30 Hz
44 Hz, если допустимо для текущего slot_count
```

и изменение частоты на лету.

Если 44 Hz физически невозможно для большой длины кадра, backend обязан корректно ограничить допустимую настройку, а не молча выдавать другую частоту.

### 24.3. Адреса

Проверить:

```text
Count=10 Start=1  -> output 1..40
Count=10 Start=21 -> output 1..60
```

### 24.4. Art-Net

Позже используется реальное Art-Net приложение на ноутбуке как источник/пульт.

Проверить прямой mapping:

```text
Art-Net channel N -> physical DMX channel N
```

Проверить short frame/long frame.

### 24.5. Art-Net reliability

При работающем ART-NET выполнить:

```text
отключить Ethernet ~0.5 s
отключить 5 s
отключить 30 s
перезагрузить Art-Net приложение/пульт
выключить и включить ноутбук/пульт
изменить IP источника
network down/up на WB
многократно повторить disconnect/reconnect
```

Во всех случаях:

- процесс `dmxwb` не требует restart;
- физический DMX не очищается из-за потери Art-Net;
- действует Hold Last;
- Source остаётся ART-NET;
- после восстановления корректные ArtDmx автоматически снова принимаются;
- sequence tracking не блокирует новый поток после перезапуска источника.

### 24.6. Source

Проверить:

```text
WB MQTT -> ART-NET
ART-NET -> WB MQTT
```

без смешанного кадра.

После reboot:

```text
mqtt -> mqtt
artnet -> artnet
```

### 24.7. Persistence

После:

```text
systemctl restart dmxwb
полного reboot WB
```

должны сохраняться:

```text
DMX Port
Refresh Rate
Art-Net Universe
Source
Fixtures
Fixture Names
Groups
Group Names/Members
Scenes
Scene Names/Snapshots
Fixture R/G/B/W
Brightness
Temperature
requested_power
```

### 24.8. Длительный тест

Не менее 24 часов непрерывной работы.

В ходе теста:

- менять RGB;
- менять Brightness;
- менять Temperature;
- применять Group;
- применять Scenes;
- переключать Source;
- использовать Art-Net;
- разрывать и восстанавливать Ethernet;
- перезапускать MQTT broker;
- проверять auto-recovery.

PASS:

- нет зависания;
- нет необходимости ручного restart;
- нет заметного мерцания, вызванного DMXWB;
- нет повреждения конфигурации;
- Art-Net восстанавливается самостоятельно;
- serial после recoverable ошибки восстанавливается автоматически.

---

## 25. Критические инварианты проекта

1. **DMX timing имеет высший приоритет.**
2. **Только DmxOutput владеет serial-портом.**
3. **MQTT callback не пишет в serial.**
4. **Art-Net callback не пишет в serial.**
5. **DMX получает только целые snapshot-ы.**
6. **Group/Scene применяются атомарно с точки зрения DMX.**
7. **Art-Net никогда автоматически не переключает Source.**
8. **Потеря Art-Net не очищает последний кадр.**
9. **Возврат Art-Net не требует restart.**
10. **Power OFF не уничтожает сохранённый цвет.**
11. **RGB/Color command устанавливает W=0.**
12. **Temperature устанавливает RGB=255 и диммирует W.**
13. **Brightness применяется ко всем четырём каналам.**
14. **Fixture Power factual state определяется реальными DMX-каналами.**
15. **Group Power factual state = OR фактических состояний участников.**
16. **Scene хранит состояния Fixture по stable ID, а не DMX address.**
17. **Удалённые Fixture/Group/Scene ID не переиспользуются.**
18. **MQTT retained не является базой конфигурации.**
19. **Все MQTT commands non-retained.**
20. **В штатном WB web видны только DMXWB Status и Source.**
21. **Собственный web взаимодействует с backend только через MQTT.**
22. **Некорректная новая конфигурация не ломает текущую рабочую.**
23. **Recoverable failure подсистемы не должен требовать restart процесса.**

---

## 26. Проверка согласованности ТЗ

При сборке документа были устранены следующие потенциальные противоречия.

### 26.1. Power command и Power status

Power command хранится внутренне как `requested_power`.

Публичный base topic `power` является factual status и вычисляется по итоговым четырём каналам.

Это позволяет одновременно:

- сохранять состояние при OFF;
- восстанавливать его при ON;
- честно показывать OFF при Brightness=0.

### 26.2. RGB state при выключенном Fixture

Base MQTT `red/green/blue/color` показывает фактический выход и равен нулю при OFF.

Сохранённый цвет не теряется: он доступен в C++ model и retained `/dmxwb/state` для собственного web.

### 26.3. Stable IDs и Scene

Fixture ID не переиспользуются после уменьшения/увеличения Count.

Иначе старая Scene могла бы случайно примениться к новому светильнику с повторно выданным ID.

### 26.4. 44 Hz и сокращённый кадр

44 Hz является протокольным потолком DMX gateway, но не гарантированно достижим при любой длине кадра с программным BREAK.

Backend обязан учитывать физическую длительность текущего кадра и не принимать невозможную частоту.

### 26.5. WB web и Fixture MQTT

Fixture/Group/Scene сохраняют MQTT state/command model, но их metadata скрыты от стандартного WB HomeUI.

В WB HomeUI остаются только `dmxwb/status` и `dmxwb/source`.

### 26.6. Art-Net reconnect

Active source lock не является вечным.

После LOST lock освобождается, sequence tracking сбрасывается, и восстановившийся источник — в том числе с новым IP — может продолжить работу без restart.

### 26.7. Art-Net short packets

Отсутствующие в ArtDmx каналы не означают zero.

Они сохраняют предыдущее Art-Net состояние, как было согласовано для непредсказуемой длины пакетов пульта.

---

## 27. Проверенные внешние основания

При проверке ТЗ использованы актуальные первичные/официальные материалы.

### Wiren Board DMX

Wiren Board публикует рабочий пример прямого DMX через встроенный RS-485 с:

- 250000 baud;
- 2 stop bits;
- BREAK через 38400 baud + `0x00`;
- непрерывной передачей.

Источник:

https://wiki.wirenboard.com/wiki/index.php?title=DMX

### Wiren Board MQTT Conventions

Проверены:

- `/devices/.../controls/...`;
- command suffix `/on`;
- retained metadata;
- `switch`;
- `range`;
- `rgb` в формате `R;G;B`;
- `text`;
- `pushbutton`;
- `hidden`;
- enum metadata.

Источник:

https://github.com/wirenboard/conventions

### Art-Net 4

Проверены:

- UDP 6454;
- 15-bit Port-Address;
- ArtDmx;
- ArtPoll/ArtPollReply;
- ArtDmx Sequence;
- Length 2..512 и чётность;
- variable-length Data;
- continuous retransmission of last DMX frame in absence of new ArtDmx;
- maximum DMX512 gateway refresh rate 44 Hz;
- multiple-source conflict may legitimately be treated as an error instead of merging.

Источник:

https://art-net.org.uk/downloads/art-net.pdf

Актуальная проверенная редакция спецификации:

```text
Art-Net 4 Protocol Release V1.4
Document Revision 1.4dp
23/10/2025
```

### MDVWB

Как архитектурный референс проверены:

- C++20;
- libmosquitto;
- CMake;
- systemd;
- serial ownership;
- MQTT command queue principle;
- retained factual state;
- static web;
- MQTT WebSocket `/mqtt`;
- web без build step и интернет-зависимостей;
- revision-protected MQTT configuration;
- atomic configuration replacement.

Источник:

https://github.com/Lex26p/MDVWB

---

## 28. Итог

Конечный DMXWB должен представлять собой один автономный C++20 daemon для Wiren Board с:

```text
1 x DMX512 output over built-in RS-485
WB MQTT Fixture/Group/Scene control
Art-Net input with automatic recovery
explicit MQTT/Art-Net source selector
static MQTT-only web UI
persistent configuration/state
systemd lifecycle
automatic subsystem recovery
```

Разработка после утверждения этого документа должна выполняться по этапам с тестовым gate после каждого значимого изменения, но конечным критерием является реализация всего настоящего ТЗ, а не промежуточной сокращённой версии продукта.
