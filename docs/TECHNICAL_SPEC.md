# Техническое задание DMXWB

**Статус документа:** согласованная рабочая спецификация для последующей разработки  
**Дата актуализации:** 2026-08-20  
**Целевая платформа:** контроллеры серии Wiren Board 8 (WB8), штатная Linux/Debian/WB software environment  
**Язык основного приложения:** C++20  
**Назначение документа:** единый источник требований к конечному приложению DMXWB.

---

## 1. Назначение системы

DMXWB — специализированное расширение возможностей контроллеров серии **Wiren Board 8 (WB8)**. Приложение реализуется как один C++20 daemon и предназначено для непрерывного управления одной физической линией DMX512 через встроенный RS-485 контроллера.

DMXWB **не заменяет Wiren Board**, не является самостоятельной SCADA, отдельным сервером автоматизации или универсальной платформой. Приложение является частью программной среды Wiren Board и использует штатную инфраструктуру контроллера там, где это предусмотрено архитектурой: локальный MQTT/Mosquitto, systemd, web/nginx-контур, локальную сеть и системные интерфейсы Linux.

Требования настоящего документа относятся к серии WB8 в целом. Реализация не должна намеренно зависеть от одной конкретной ревизии контроллера или одной конкретной версии ПО WB, если такая зависимость не обусловлена реальным аппаратным или системным ограничением. При hardware/integration acceptance всегда фиксируются конкретная модель WB8 и версия установленного ПО, на которых выполнялась проверка.

Главная цель системы — надёжное управление реальным освещением по DMX512. WB MQTT обеспечивает локальное логическое управление светом средствами Wiren Board, а ART-NET — внешнее сетевое управление тем же физическим DMX-выходом. Web, persistence, diagnostics и deployment обслуживают эту основную функцию и не должны снижать стабильность физического DMX output.

Приложение поддерживает два независимых источника данных:

1. **WB MQTT** — логическое управление RGBW-светильниками, группами и сценами.
2. **ART-NET** — прямое управление физическими DMX-каналами через Ethernet.

В каждый момент времени физический DMX-выход использует только один выбранный источник.

Переключение источника выполняется явно через MQTT. Автоматического переключения между WB MQTT и ART-NET нет.

Веб-интерфейс DMXWB является локальным средством настройки, диагностики и ручного управления приложением. Он не является системой диспетчеризации. Веб-интерфейс полностью статический и взаимодействует с C++-приложением исключительно через MQTT.

---

## 2. Архитектура верхнего уровня

```text
                    Wiren Board ecosystem
                            |
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
                /dev/ttyRS485-*
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
- web не имеет отдельного REST/HTTP API;
- C++ backend является источником истины для конфигурации и состояния DMXWB;
- DMXWB использует штатную инфраструктуру Wiren Board и не дублирует функции контроллера без необходимости;
- работа DMXWB не зависит от внешнего интернета;
- Docker не является частью build/runtime/deployment архитектуры DMXWB.

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

Поддерживаются встроенные RS-485-порты WB8, доступные штатной Linux-системе как:

```text
/dev/ttyRS485-1
/dev/ttyRS485-2
```

Значение по умолчанию:

```text
/dev/ttyRS485-1
```

В каждый момент приложение использует только один порт.

Конкретная доступность и именование порта должны подтверждаться на тестируемой модели WB8 и версии ПО. Если в поддерживаемой конфигурации WB8 интерфейс отличается, это должно быть оформлено как явная совместимость, а не как неявная привязка всего приложения к одной модели.

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

### 4.3. BREAK и управление передатчиком RS-485

Предпочтительный production-путь на встроенном RS-485 WB8 использует штатные Linux UART/TTY ioctl и не требует custom kernel patch. Если порт предоставляет необходимые возможности, `DmxTransport` должен:

```text
сохранить исходную serial_rs485 конфигурацию
временно отключить automatic kernel RS-485 direction control
оставить line mode = 250000 8N2
поднять DE вручную через RTS
TIOCSBRK
удерживать BREAK не менее 120 us
TIOCCBRK
выдержать Mark After Break не менее 20 us
передать Start Code 0x00 + active slots
дождаться физического transmitter empty (TEMT)
опустить DE через RTS
```

Ожидание окончания кадра должно подтверждать физическое завершение передачи, а не только опустошение userspace buffer. На доказанном WB8 fast path используется `TIOCSERGETLSR`/`TIOCSER_TEMT`; перед закрытием порта, при normal stop и при error/recovery исходная `serial_rs485` конфигурация восстанавливается.

Capability определяется для фактически открытого serial-порта. Реализация не должна считать наличие fast path свойством только одной ревизии WB8.

Если необходимые стандартные Linux ioctl недоступны, сохраняется compatibility fallback, доказанный DEV-003:

```text
38400 8N2
-> передача 0x00
-> wait/drain до физического завершения BREAK byte
-> 250000 8N2
-> Start Code 0x00 + active slots
-> wait/drain
```

Fast path подтверждён на acceptance-конфигурации WB8 rev. 8.5.1 (T507), kernel `6.8.0-wb160`. После DEV-004 два последовательных 60-секундных production run `300 slots / 44 Hz` дали `2640/2640`, `missed_deadlines=0` и no visible flicker; максимальный наблюдавшийся send time — `17.689 ms` при периоде `22.727 ms`. Для этой конфигурации custom kernel patch не требуется.

При наличии безопасного измерительного оборудования BREAK/MAB/bit timing дополнительно допускается проверить логическим анализатором или осциллографом с безопасной схемой подключения.

Переход на собственный kernel/WBEC DMX engine остаётся отдельным архитектурным изменением и требует пересмотра ТЗ.

### 4.4. Непрерывность передачи

DMX передаётся непрерывно независимо от того, меняются значения или нет.

MQTT/Art-Net обновляют только внутренние данные. Физический DMX-цикл работает самостоятельно.

### 4.5. Физический лимит slots

DMXWB использует стандартный DMX512 physical layer, но намеренно ограничивает production output первыми **300 physical slots**.

```text
kDmxMaxChannels       = 512   // core / Art-Net data capacity
kDmxPhysicalMaxSlots  = 300   // physical RS-485 product limit
```

Это продуктовый профиль DMXWB, а не отдельный протокол «DMX300».

В WB MQTT последний физический адрес Fixture не может превышать 300. Неиспользуемые каналы внутри активного физического диапазона равны нулю.

Art-Net subsystem хранит все 512 сетевых каналов для protocol compatibility, но source selector формирует физический snapshot только из channels `1..300`; channels `301..512` могут сохраняться в `artnet_state` и никогда не передаются в этот RS-485 output.

Фактический физический `slot_count` может быть меньше 300, но никогда больше 300. Worst-case timing guarantee проверяется на полном 300-slot кадре.

При `Fixture Count = 0` пользовательских Fixture slots нет; правила startup/source switching определяют, когда physical output получает первый snapshot.

### 4.6. Refresh Rate

Production physical DMX cadence фиксирована:

```text
44 Hz
```

Refresh Rate **не является пользовательской или persistent настройкой**. Backend, MQTT и Web не предлагают выбор частоты.

Период задаётся между началами кадров:

```text
T0
T0 + 1/44 s
T0 + 2/44 s
...
```

Отправка текущего кадра не добавляется к следующему period. При scheduler/OS delay прошедшие deadlines считаются в diagnostics и абсолютная grid сохраняется.

Решение основано на worst-case physical limit 300 slots. Два последовательных минутных production run `300/44` на acceptance WB8 прошли без missed deadlines и flicker. Проверка `512/40` дала один missed deadline за минуту и поэтому не используется как product profile.

Гарантия `<=300 slots / 44 Hz` относится к подтверждённому fast transport path. Legacy DEV-003 fallback остаётся compatibility mechanism, но сам по себе не является доказательством production 44 Hz profile на неизвестном WB8 UART implementation.

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

Публикуемый RGB status показывает значения после Brightness и Power:

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

Сохранённые значения продолжают существовать во внутреннем состоянии и восстанавливаются при включении.

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
start_address + count * 4 - 1 <= 300
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

Отдельного группового слоя mixing нет. Последняя применённая команда определяет состояние конкретного Fixture.

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

При удалении Fixture его ID автоматически удаляется из всех Group. Пустая Group остаётся существовать.

### 7.7. Удаление Group

Удаление Group не меняет состояния её бывших участников.

### 7.8. ID и Name

Group ID постоянный и не переиспользуется после удаления.

Name изменяемый, текстовый и не обязан быть уникальным.

---

## 8. Сцены

Scene — сохранённый snapshot индивидуального состояния светильников. Она не является Group и не является отдельным DMX-слоем.

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

Scene не зависит от DMX-адресов и не хранит историю способа управления.

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

Если после сохранения сцены появились новые Fixture, Scene их не изменяет.

Если Fixture из snapshot больше не существует, его запись игнорируется.

Удалённые Fixture ID не переиспользуются.

### 8.5. Source

Apply Scene не переключает Source.

При выбранном ART-NET сцена изменяет MQTT-состояние и `mqtt_buffer`, но физический выход остаётся Art-Net.

### 8.6. ID и Name

Scene ID постоянный и не переиспользуется. Name изменяемый и используется интерфейсами отображения.

---

## 9. MQTT: общие правила

MQTT broker на целевом WB8:

```text
127.0.0.1:1883
```

C++ использует `libmosquitto`.

MQTT не является базой конфигурации.

Источник истины для данных DMXWB:

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

`reset` — stateless pushbutton.

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

Payload содержит полную каноническую конфигурацию и поле `revision`.

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
3. проверяет ссылки и диапазоны;
4. проверяет DMX addresses;
5. строит новую внутреннюю конфигурацию;
6. атомарно записывает `config.json`;
7. увеличивает revision;
8. одним действием применяет новую конфигурацию;
9. публикует новый retained `/dmxwb/config`;
10. публикует result.

Если `expected_revision` устарел, save отклоняется.

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

State содержит сохранённое логическое состояние:

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

Он нужен, в частности, чтобы web мог помнить сохранённый цвет выключенного светильника.

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

Команды:

```text
/dmxwb/scenes/create
/dmxwb/scenes/<scene_id>/overwrite
/dmxwb/scenes/<scene_id>/delete
```

Все команды `retain=false` и содержат `request_id`.

### 14.7. Live controls

Обычные команды света собственного web идут через те же `/devices/.../controls/.../on`, что и любые внешние MQTT-клиенты.

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

Web использует штатную инфраструктуру WB и MQTT как runtime IPC. В рамках DMXWB не проектируются собственные users/authentication/authorization, Mosquitto ACL или отдельная модель доступа к `/mqtt`; это относится к общей конфигурации контроллера и сети.

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

### 15.4. Slider publishing

Во время движения slider команды передаются плавно.

Web ограничивает частоту публикации одного control примерно до 20–30 команд/с и обязательно отправляет последнее значение.

### 15.5. Factual confirmation

Web не считает команду выполненной только потому, что MQTT publish прошёл.

После `/on` интерфейс получает подтверждённое состояние через base topic и `/dmxwb/state`.

### 15.6. Структурные настройки

Live controls применяются сразу.

Структурные настройки применяются только после явного `Применить`.

К структурным относятся:

```text
DMX Port
Fixture Count
Start Address
Group membership
Art-Net Universe
```

До Apply web работает с локальным draft.

### 15.7. Reconnect браузера

При потере MQTT:

- показать «Нет связи с DMXWB»;
- заблокировать новые команды;
- автоматически переподключаться;
- после reconnect повторно подписаться;
- получить retained config/state/status;
- не переотправлять старые команды.

Reload страницы не должен требоваться.

---

## 16. Art-Net

### 16.1. Общие параметры

```text
Art-Net 4
UDP 6454 (0x1936)
IPv4
one output Port-Address
```

Пользовательская настройка `Art-Net Universe` хранит 15-bit Port-Address.

Текущая Art-Net 4 specification определяет стандартный диапазон `1..32767` и помечает `0` deprecated. DMXWB сознательно сохраняет `0..32767`, default `0`, только как compatibility exception для распространённых zero-based controllers; Web/diagnostics должны явно обозначать значение 0 как legacy compatibility.

### 16.2. Обязательные packets DMXWB

```text
ArtDmx
ArtPoll
ArtPollReply
ArtSync
```

ArtPoll/ArtPollReply являются частью universe subscription, а не только декоративным discovery.

### 16.3. ArtPoll / ArtPollReply

DMXWB объявляет один Art-Net output port и **всегда** публикует настроенный output Port-Address в `SwOut`, даже когда physical Source = WB MQTT. Это позволяет корректному Art-Net controller продолжать считать DMXWB subscriber и обновлять background `artnet_state`.

`ArtPollReply.RefreshRate`:

```text
44 Hz
```

`GoodOutputA.bit7` устанавливается только когда ArtDmx data действительно выбрана и выводится как physical DMX; при Source=MQTT этот bit очищен, хотя `SwOut` subscription остаётся объявленной.

На ArtPoll reply отправляется unicast. Для масштабируемости используется случайная задержка до 1 s. Targeted Mode поддерживается: если включённый диапазон ArtPoll не содержит настроенный Port-Address, DMXWB не отвечает.

### 16.4. ArtDmx validation и extensibility

Принимаются только packets с:

- signature `Art-Net\0`;
- OpCode = ArtDmx;
- protocol revision >= 14;
- нужным Port-Address;
- `Length` even `2..512`;
- фактическим UDP payload не короче обязательного header + declared Data.

Для ArtDmx минимально требуется `18 + Length` bytes. Дополнительные trailing bytes валидного будущего расширения **игнорируются**, а не вызывают reject. Неиспользуемые/reserved bits не тестируются receiver-ом, если specification требует transmit-as-zero / receiver-do-not-test semantics.

Актуальная specification требует unicast ArtDmx только subscribed nodes и запрещает controller broadcast ArtDmx. DMXWB conformance behavior основан на unicast subscription. Receiver может принимать корректный legacy broadcast packet как compatibility input, но production interoperability не должна зависеть от broadcast.

### 16.5. Сетевое состояние 512 и физический output 300

Art-Net хранит:

```text
artnet_state[512]
```

Если `Length < 512`, обновляются только channels `1..Length`; остальные сохраняют прежние значения (Hold Last per channel).

Если packet содержит channels `301..512`, они валидируются и сохраняются в `artnet_state`, но physical source snapshot использует только channels `1..300`.

### 16.6. Output cadence / latest snapshot wins

ArtDmx arrival **никогда не запускает serial transmission**.

```text
ArtDmx -> update latest committed Art-Net state
DMX frame boundary @ fixed 44 Hz -> acquire latest whole physical snapshot
```

ArtDmx frames не ставятся в FIFO. Если между двумя physical boundaries пришло несколько valid updates, промежуточные snapshots superseded более новым. Это предотвращает накопительную latency, когда network input временно быстрее physical output или приходит burst-ами.

### 16.7. ArtSync

После power-on/restart DMXWB работает asynchronous: valid ArtDmx сразу обновляет committed Art-Net state.

После получения valid ArtSync от IP текущего ArtDmx source DMXWB переходит в synchronous mode:

```text
ArtDmx -> staging state
next ArtSync -> staging atomically becomes committed state
next physical DMX boundary -> latest committed snapshot
```

ArtSync не запускает UART напрямую и не изменяет fixed 44 Hz clock.

Если ArtSync не приходит >= 4 s, DMXWB возвращается в asynchronous mode. ArtSync от IP, не совпадающего с relevant/most-recent ArtDmx source IP, игнорируется.

### 16.8. Sequence

`Sequence = 0` отключает sequence checking.

Для `1..255` newer packets могут заменять state, stale/out-of-order packets не перезаписывают newer state; rollover `0xFF -> 0x01` учитывается. DMXWB не ждёт отсутствующий sequence number и не создаёт очередь network frames.

Tracking сбрасывается при освобождении active source lock.

### 16.9. Source identity и conflict

ArtDmx source identity:

```text
source IPv4 + Physical
```

`Physical` нужен, потому что один IP может представлять несколько физических DMX input ports.

DMXWB сознательно выбирает разрешённую specification policy **error/conflict вместо automatic HTP/LTP merge**:

```text
нет active source -> первый valid source становится ACTIVE
тот же source     -> packets принимаются
другой IP или другой Physical на том же Port-Address
                   -> CONFLICT; second source data не применяется
```

Policy должна быть отражена в user documentation. HTP/LTP merge не реализуется.

### 16.10. Hold Last и LOST

При прекращении ArtDmx:

- `artnet_state` не очищается;
- committed physical snapshot остаётся последним корректным;
- Source не переключается автоматически;
- Blackout не генерируется.

Diagnostic/source-lock timeout DMXWB:

```text
3 seconds
```

Specification рекомендует active-but-unchanged ArtDmx source повторять last packet примерно каждые 800–1000 ms, поэтому 3 s даёт запас для network jitter и одновременно позволяет освобождать stale source lock.

После LOST sequence/sync tracking для освобождённого source сбрасывается; следующий valid source может стать ACTIVE.

### 16.11. Network recovery

Art-Net subsystem автоматически восстанавливается после Ethernet disconnect/reconnect, source restart/power cycle, IP change, WB network down/up и временной UDP socket error. Socket при необходимости пересоздаётся и bind-ится повторно на UDP 6454.

### 16.12. OEM / credits

До production release DMXWB должен получить зарегистрированный **Art-Net OEM Code** от Artistic Licence. Не назначать произвольный production OEM Code самостоятельно.

User documentation продукта, реализующего Art-Net, должна содержать требуемый Art-Net copyright credit. Development tests могут использовать явно обозначенный placeholder только до получения реального OEM Code; production bundle с placeholder не выпускается.

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

Реальная реализация может объединять Main/Persistence, но DMX Output остаётся отдельным рабочим контекстом.

### 17.3. MQTT callback

Callback:

1. проверяет topic;
2. парсит payload;
3. валидирует базовый формат;
4. формирует Command;
5. помещает Command в thread-safe queue.

Callback не изменяет serial и не выполняет длительные файловые операции.

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

Controller не даёт DMX thread mutable-ссылку на Fixture model.

После изменения строится целый:

```text
DmxSnapshot
```

с:

```text
channels[]
slot_count
generation/revision
```

Передача выполняется immutable/double-buffer или эквивалентным способом, не позволяющим увидеть частично изменённый snapshot.

### 17.6. Art-Net snapshot

Art-Net thread хранит persistent `artnet_state` и после корректного пакета публикует целый новый snapshot.

### 17.7. Scene/Group atomicity

Group/Scene сначала целиком меняет внутреннюю модель, затем публикует один новый DMX snapshot.

### 17.8. Приоритет DMX над MQTT publication

После логической команды:

```text
1. обновить внутреннюю модель
2. построить и опубликовать DMX snapshot
3. поставить MQTT publications
```

Физический свет не ждёт публикации MQTT state сообщений.

---

## 18. Конфигурация и persistence

### 18.1. Файлы

```text
/etc/dmxwb/config.json
/var/lib/dmxwb/state.json
```

### 18.2. config.json

Содержит минимум:

```text
version
revision
DMX: port
Art-Net: universe
Fixtures: ordered records, id, name, count, start_address
Groups: id, name, member fixture IDs
Scenes: id, name, fixture snapshots
ID counters: next_fixture_id, next_group_id, next_scene_id
```

### 18.3. state.json

Содержит:

```text
version
source
Fixtures:
    id
    requested_power
    R/G/B/W
    brightness
    temperature
```

Art-Net transient buffer не используется для восстановления логической MQTT-модели.

### 18.4. Асинхронная запись state

После runtime изменения:

```text
dirty = true
```

Запись:

```text
через 2 s после последнего изменения
ИЛИ
не реже одного раза в 10 s при непрерывных изменениях
```

### 18.5. Atomic file replace

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
- сохранить dirty state;
- остановить workers;
- закрыть serial и UDP;
- корректно disconnect MQTT;
- завершить процесс.

### 18.7. Ошибка state.json

При corrupt `state.json` config остаётся рабочим, создаются безопасные default Fixture states, ошибка диагностируется, приложение продолжает работу.

### 18.8. Ошибка config.json

Повреждённый config автоматически не перезаписывается.

В памяти запускаются defaults:

```text
DMX Port = /dev/ttyRS485-1
Art-Net Universe = 0
Fixture Count = 0
Start Address = 1
```

Публикуется Configuration Error; пользователь может сохранить новую корректную конфигурацию.

---

## 19. systemd и жизненный цикл

Service:

```text
Type=simple
Restart=on-failure
RestartSec=2s
```

Сервис не должен требовать наличия Mosquitto как условия запуска DMX-процесса.

Recoverable failures восстанавливаются внутри приложения:

```text
MQTT lost    -> reconnect
Art-Net lost -> Hold Last + auto recovery
RS-485 lost  -> reopen/retry
network lost -> auto recovery
```

systemd restart применяется только при реальном завершении/аварии процесса.

### 19.1. MQTT reconnect

После reconnect:

1. восстановить subscriptions;
2. опубликовать metadata;
3. опубликовать `/devices/dmxwb`;
4. опубликовать Fixture/Group/Scene states;
5. опубликовать `/dmxwb/config`;
6. опубликовать `/dmxwb/state`;
7. опубликовать `/dmxwb/status`.

Retained MQTT commands не выполняются.

---

## 20. Ошибки и диагностика

Уровни:

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

Если Source = ART-NET и Art-Net LOST: общий Status = error.

Если Source = WB MQTT и Art-Net LOST, но MQTT и DMX работают: общий Status = running, Art-Net = warning.

### 20.2. DMX diagnostics

Минимально:

```text
state
port
slot_count
refresh_hz = 44
physical_slot_limit = 300
frames_sent
last_error
recovery_state
```

### 20.3. Art-Net diagnostics

```text
state
universe
active_source_ip
active_source_physical
last_packet_age
last_sequence
sync_mode = async / sync
last_sync_age
conflicting_source_ip
conflicting_source_physical
output_mode = Hold Last / Live
packets_received
snapshots_superseded
recovery_state
```

### 20.4. MQTT diagnostics

```text
Connected / Disconnected
Reconnecting
```

### 20.5. Config errors

Некорректная новая конфигурация отклоняется целиком; старая продолжает работать; web получает сообщение; частичного применения нет.

### 20.6. Логи

Приложение пишет stdout/stderr; основной журнал:

```text
journalctl -u dmxwb
```

Не логируются каждый DMX frame и каждое промежуточное движение slider.

Логируются startup/shutdown, config load/save/reject, serial open/lost/recovered, MQTT connect/lost/recovered, Art-Net source events, Scene apply и fatal errors.

---

## 21. Структура проекта

Целевая структура:

```text
DMXWB/
├── CMakeLists.txt
├── CMakePresets.json
├── README.md
├── AGENTS.md
├── src/
│   ├── main.cpp
│   ├── controller/
│   ├── model/
│   ├── dmx/
│   ├── artnet/
│   ├── mqtt/
│   ├── config/
│   └── persistence/
├── include/
├── www/
│   └── dmxwb/
├── deploy/
│   ├── dmxwb.service
│   ├── install_wirenboard.sh
│   └── config.example.json
├── tests/
└── docs/
```

Фактическая структура развивается по gates; этот раздел задаёт конечное направление, а не обязанность заранее создавать пустые каталоги.

---

## 22. Зависимости, среда разработки и сборка

### 22.1. Runtime/build зависимости

Обязательные технологии конечного приложения:

```text
C++20
CMake
Threads
libmosquitto
POSIX serial/termios
POSIX UDP sockets
systemd на целевом WB8
```

JSON-реализация должна быть компактной и пригодной для offline deployment на поддерживаемые WB8.

Production-сборка должна завершаться ошибкой, если отсутствует обязательная MQTT-зависимость, а не создавать частично работоспособный бинарник.

### 22.2. Среда разработки

Основная пользовательская development environment:

```text
Windows
Visual Studio 2026
C:\Projects\DMXWB
```

На ноутбуке пользователя также установлена локальная Linux-среда.

Разделение:

- Windows / Visual Studio 2026 — основная работа с проектом, MSVC/CMake host build и host unit tests;
- локальный Linux на ноутбуке — Linux/POSIX build checks, целевая сборка для WB8 и подготовка production artifacts;
- WB8 — runtime, hardware/integration и acceptance target.

### 22.3. Целевая production-сборка

Production binary собирается **на ноутбуке пользователя**, а не на контроллере WB8.

Конкретный non-Docker способ получения бинарника для целевой архитектуры WB8 должен быть подтверждён практически и зафиксирован в `PROJECT_STATE.md`. Это может быть подходящий cross compiler/sysroot или иная локальная Linux toolchain, если она выдаёт совместимый бинарник.

До использования конкретного toolchain необходимо подтвердить:

- целевую архитектуру тестируемого WB8;
- ABI/glibc и необходимые runtime libraries;
- совместимость `libmosquitto` и JSON dependency;
- возможность воспроизводимой сборки на ноутбуке;
- успешный запуск полученного binary на WB8.

Не допускается скрытая привязка production build к одной тестовой модели без проверки применимости к остальным поддерживаемым WB8.

### 22.4. Docker

**Docker не используется.**

Docker не является:

- build dependency;
- способом cross-compilation;
- runtime dependency;
- deployment layer;
- обязательной test environment.

Изменение этого решения возможно только после явного пересмотра требований пользователем.

### 22.5. Офлайн-установка на WB8

Установка конечного DMXWB на поддерживаемый WB8 выполняется полностью офлайн, без доступа в интернет.

Финальный installation bundle содержит готовый production binary `dmxwb`, статические web-файлы, systemd unit, deployment-файлы и все локальные runtime-файлы, которые реально требуются.

Production installer:

- не выполняет `apt update` или online `apt install`;
- не выполняет `git clone`, `curl`, `wget` или иные загрузки;
- не требует npm/Node.js/frontend build;
- не требует C++ compiler или CMake на WB8;
- не компилирует исходники DMXWB на WB8;
- использует только гарантированно присутствующие системные компоненты либо локально поставляемые совместимые зависимости.

После установки и reboot DMXWB выполняет основные функции без внешнего интернета. Для эксплуатации используется только штатная инфраструктура контроллера и локальная сеть, необходимая для MQTT/Web/Art-Net.

---

## 23. Тесты

### 23.1. Unit

Обязательно покрываются:

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
physical slot limit <=300
fixed 44 Hz cadence
Fixture config validation
stable IDs
Group operations
Group power aggregation
Scene snapshot/apply
Scene with missing/new Fixtures
ArtDmx parser/minimum length/trailing extension bytes
ArtDmx even Length 2..512
short ArtDmx per-channel Hold Last
512 network state -> 300 physical truncation
Sequence/rollover/no-wait ordering
ArtSync staging/release/4 s async fallback
ArtPoll/ArtPollReply subscription + Targeted Mode + RefreshRate 44
source identity IP+Physical
active source timeout
source conflict without merge
Source selector
config parse/serialize
state parse/serialize
revision conflict
atomic update behavior
```

### 23.2. MQTT integration

Проверяется цепочка:

```text
/on command
-> C++ state
-> DMX snapshot
-> published state
```

Retained states доступны новому клиенту. Retained `/on` игнорируется. Удаление Fixture/Group/Scene очищает устаревшие retained topics.

### 23.3. Web

Проверяется:

- загрузка без внешнего интернета;
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
- backend validation errors;
- восстановление UI после reload/reconnect.

### 23.4. Target build

До финального deployment должен существовать воспроизводимый non-Docker build path на ноутбуке:

```text
Windows / Visual Studio 2026 -> host validation
Local Linux                 -> Linux/target build
WB8                         -> run/integration only
```

PASS:

- целевой binary получен на ноутбуке;
- на WB8 для его получения не устанавливаются compiler/CMake;
- binary запускается на проверяемой модели WB8;
- зависимости либо уже штатно доступны, либо входят в offline bundle.

### 23.5. Offline installation

На поддерживаемом WB8 с физически недоступным внешним интернетом проверяется финальный installation bundle.

Проверяется:

- установка только из локального bundle;
- отсутствие online package/download steps;
- отсутствие C++/CMake/Node.js toolchain на target;
- systemd start после установки и reboot;
- web из локальной LAN;
- backend -> локальный Mosquitto;
- базовый физический DMX output;
- Art-Net из локальной сети.

---

## 24. Hardware acceptance

Все hardware acceptance tests выполняются на реальном контроллере серии WB8. В отчёте/`PROJECT_STATE.md` фиксируются:

```text
WB8 model
hardware revision, если доступна и существенна
WB software / OS version
DMX port
build/toolchain identity
```

Проверка на одной конкретной модели является подтверждением этой тестовой конфигурации; при выявлении model-specific различий требования совместимости уточняются отдельно, не сужая молча весь target до одной версии.

### 24.1. DMX

На реальном WB8 и RGBW-светильнике:

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

Проверить 10 Hz, 30 Hz и 44 Hz, если текущий slot_count/transport физически допускает 44 Hz, а также изменение частоты на лету.

### 24.3. Адреса

```text
Count=10 Start=1  -> output 1..40
Count=10 Start=21 -> output 1..60
```

### 24.4. Art-Net

Реальное Art-Net приложение на ноутбуке используется как источник/пульт.

Проверить:

```text
Art-Net channel N -> physical DMX channel N
short frame
long frame
```

### 24.5. Art-Net reliability

При активном ART-NET:

```text
Ethernet disconnect ~0.5 s
5 s
30 s
restart Art-Net application
power cycle source where applicable
IP change
WB8 network down/up
repeated disconnect/reconnect
```

Во всех случаях:

- процесс `dmxwb` не требует restart;
- действует Hold Last;
- Source остаётся ART-NET;
- после восстановления ArtDmx автоматически принимаются;
- sequence/source lock не мешают recovery.

### 24.6. Source

Проверить:

```text
WB MQTT -> ART-NET
ART-NET -> WB MQTT
```

без смешанного кадра.

После reboot Source восстанавливается из persistence.

### 24.7. Persistence

После `systemctl restart dmxwb` и полного reboot должны сохраняться согласованные config/state данные: порт, Art-Net universe, Source, Fixtures, names, Groups, Scenes, RGBW, Brightness, Temperature и requested_power.

### 24.8. Длительный тест

Не менее 24 часов непрерывной работы с изменением Fixture/Group/Scene, source switching, Art-Net, network loss/recovery и MQTT broker restart.

PASS:

- нет зависания;
- нет необходимости ручного restart;
- нет заметного flicker от DMXWB;
- нет повреждения конфигурации;
- Art-Net восстанавливается;
- serial recoverable failure восстанавливается автоматически.

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
15. **Group Power = OR фактических состояний участников.**
16. **Scene хранит Fixture по stable ID, а не DMX address.**
17. **Удалённые Fixture/Group/Scene ID не переиспользуются.**
18. **MQTT retained не является базой конфигурации.**
19. **Все MQTT commands non-retained.**
20. **В штатном WB web видны только DMXWB Status и Source.**
21. **Собственный web взаимодействует с backend только через MQTT.**
22. **Некорректная новая конфигурация не ломает текущую рабочую.**
23. **Recoverable failure подсистемы не требует restart процесса.**
24. **DMXWB является расширением Wiren Board, а не заменой контроллера/SCADA.**
25. **Установка и работа DMXWB не зависят от внешнего интернет-доступа.**
26. **Authentication/authorization/ACL инфраструктуры WB находятся вне scope DMXWB.**
27. **Целевая платформа — серия WB8, а не одна фиксированная версия контроллера.**
28. **Production binary собирается на ноутбуке, а WB8 используется как runtime/hardware target.**
29. **Docker не используется.**

---

## 26. Проверка согласованности ТЗ

### 26.1. Power command и Power state

`requested_power` хранит логическое желание включения, а публичный Power вычисляется по итоговым четырём каналам Fixture model. Это позволяет сохранять состояние при OFF и честно показывать OFF при Brightness=0.

### 26.2. RGB state при выключенном Fixture

MQTT `red/green/blue/color` для Fixture model равен нулю при OFF. Сохранённый цвет остаётся в C++ model и `/dmxwb/state`.

### 26.3. Stable IDs и Scene

Fixture ID не переиспользуются после уменьшения/увеличения Count, чтобы старая Scene не применялась к новому устройству случайно.

### 26.4. 44 Hz и сокращённый кадр

DMXWB physical profile ограничен 300 slots и фиксирован на 44 Hz. Внутренние DMX/Art-Net structures сохраняют 512 channels; physical output никогда не передаёт channel >300.

### 26.5. WB web и Fixture MQTT

Fixture/Group/Scene сохраняют MQTT state/command model, но скрыты от стандартного WB HomeUI metadata `hidden=true`.

### 26.6. Art-Net reconnect

После LOST active-source lock освобождается, sequence tracking сбрасывается, источник может восстановиться без restart.

### 26.7. Art-Net short packets

Отсутствующие в ArtDmx каналы означают Hold Last, а не zero.

### 26.8. Серия WB8 и конкретный acceptance target

Требование «WB8 series» означает, что проект не проектируется под одну заранее зафиксированную модель/версию. При этом аппаратное доказательство всегда выполняется на конкретном контроллере, и его модель/версия записываются для воспроизводимости.

### 26.9. Build host и target

Windows/Visual Studio 2026 и локальный Linux на ноутбуке являются development/build средой. WB8 не является обязательной compile machine и получает готовые artifacts.

---

## 27. Проверенные внешние основания

Используются первичные/официальные материалы по мере необходимости и повторно проверяются перед этапами, зависящими от изменяемой внешней документации.

### Wiren Board DMX

Рабочий подход прямого DMX через встроенный RS-485:

- 250000 baud;
- 2 stop bits;
- BREAK через 38400 baud + `0x00`;
- continuous repeated frames.

Источник: `https://wiki.wirenboard.com/wiki/index.php?title=DMX`

### Wiren Board MQTT Conventions

Проверяются `/devices/.../controls/...`, `/on`, retained metadata, `switch`, `range`, `rgb`, `text`, `pushbutton`, `hidden`, enum metadata.

Источник: `https://github.com/wirenboard/conventions`

### Art-Net 4

Проверяются UDP 6454, Port-Address, unicast subscription, ArtDmx, ArtPoll/ArtPollReply, ArtSync, Sequence, Physical source identity, Length/minimum packet size, trailing extension bytes, Hold Last, RefreshRate=44 и documented CONFLICT multiple-source policy.

Источник: `https://art-net.org.uk/downloads/art-net.pdf`

### MDVWB

Может использоваться как архитектурный референс для C++/libmosquitto/systemd/static web/MQTT patterns, но не заменяет требования DMXWB.

Источник: `https://github.com/Lex26p/MDVWB`

---

## 28. Итог

Конечный DMXWB — специализированный C++20 daemon, устанавливаемый как **расширение возможностей контроллеров серии WB8**, с:

```text
1 x DMX512 output over built-in RS-485
WB MQTT Fixture/Group/Scene control
Art-Net input with automatic recovery
explicit MQTT/Art-Net source selector
static MQTT-only web UI
persistent configuration/state
systemd lifecycle
automatic subsystem recovery
fully offline installation on supported WB8
```

Production binary и installation bundle подготавливаются на ноутбуке пользователя с использованием Windows/Visual Studio 2026 для host-разработки и локального Linux для Linux/target build. Docker не используется. Контроллер WB8 получает готовые artifacts и служит runtime/hardware/integration target.

Главный конечный критерий — после полностью офлайн-установки на поддерживаемый WB8 DMXWB стабильно управляет реальным освещением через физический DMX512 и принимает внешнее управление по Art-Net, сохраняя локальную интеграцию со штатной экосистемой Wiren Board.

Разработка выполняется по этапам с проверяемыми gates согласно `ROADMAP.md`. Промежуточные документационные/организационные шаги могут выполняться внутри текущего engineering gate и сами по себе не означают PASS hardware/functionality gate.
