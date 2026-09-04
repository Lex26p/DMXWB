# DMXWB — установка, обновление и удаление на WB8

Эта инструкция предназначена для конечного пользователя. Репозиторий проекта,
исходный код, WSL, компилятор, CMake, Node.js, Docker и внешний Интернет для
установки не нужны.

## 1. Установочный пакет

Пользователь получает один готовый архив:

```text
dmxwb-0.1.1-wb8-bullseye-arm64.tar.gz
```

Проверенный SHA256:

```text
42e440034dcacdfdb1cfff5ed3fb54bc2cc367b5a70200aaaba9a5924c09ef3d
```

Распакуйте архив на компьютере обычным архиватором. Получится готовая папка:

```text
dmxwb-wb8-bullseye-arm64
```

В ней уже находятся `install.sh`, `uninstall.sh`, готовая программа, служба systemd
и Web-интерфейс. Ничего отдельно искать, собирать или раскладывать не нужно.

Пакет проверен на Wiren Board 8 с Debian 11 Bullseye, архитектурой ARM64/AArch64 и
WB release `wb-2606 stable`.

## 2. Копирование пакета на контроллер

1. Подключитесь к WB8 через WinSCP или другой файловый клиент.
2. Скопируйте **всё содержимое** распакованной папки
   `dmxwb-wb8-bullseye-arm64` в папку контроллера:

   ```text
   /root/dmxwb-installer
   ```

3. Проверьте, что итоговая структура начинается именно так:

   ```text
   /root/dmxwb-installer/install.sh
   /root/dmxwb-installer/uninstall.sh
   /root/dmxwb-installer/MANIFEST.txt
   /root/dmxwb-installer/SHA256SUMS
   /root/dmxwb-installer/payload/
   ```

Не нужно вручную копировать программу в `/usr/local/bin`, создавать службу или
раскладывать Web-файлы. Всё это делает установочный скрипт.

Папку `/root/dmxwb-installer` рекомендуется сохранить: из неё выполняются
обновление и удаление.

## 3. Первоначальная установка

1. Подключитесь к контроллеру:

   ```bash
   ssh root@10.200.200.1
   ```

   Если адрес WB8 другой, используйте его фактический IP.

2. Запустите установку:

   ```bash
   bash /root/dmxwb-installer/install.sh
   ```

Скрипт сам проверит пакет и платформу, установит готовую программу, systemd unit и
Web, создаст начальную конфигурацию только при её отсутствии, включит автозапуск и
запустит DMXWB.

Успешная первая установка заканчивается сообщением:

```text
DMXWB fresh installation completed.
config_path=/etc/dmxwb/config.json
state_path=/var/lib/dmxwb/state.json
service=enabled_and_started
```

После установки откройте `http://10.200.200.1/dmxwb/`, выполните `Ctrl+F5` и
настройте приложение по [инструкции Web](WEB_USER_GUIDE.md).

### Проверка установки

Команды проверки выполняются отдельно и ничего не изменяют:

```bash
systemctl is-enabled dmxwb.service
systemctl is-active dmxwb.service
systemctl status dmxwb.service --no-pager -l
/usr/local/bin/dmxwb --version
test -x /usr/local/bin/dmxwb && echo 'Программа установлена'
test -s /etc/dmxwb/config.json && echo 'Конфигурация существует'
test -s /var/www/dmxwb/index.html && echo 'Web установлен'
mosquitto_sub -h 127.0.0.1 -p 1883 -C 1 -t /devices/dmxwb/controls/status
```

Ожидаются `enabled`, `active`, версия DMXWB, три подтверждающие строки и статус
`running`. При фактической ошибке DMX-порта или конфигурации служба остаётся
доступной со статусом `error`; причина показывается в Web и журнале.

## 4. Обновление

Обновление выполняет тот же `install.sh`. Он сам остановит службу, заменит только
файлы приложения и снова её запустит. Следующие пользовательские файлы сохраняются:

```text
/etc/dmxwb/config.json
/var/lib/dmxwb/state.json
```

Порядок обновления:

1. Получите новый готовый архив и распакуйте его на компьютере.
2. Через WinSCP полностью замените содержимое `/root/dmxwb-installer` содержимым
   новой готовой папки `dmxwb-wb8-bullseye-arm64`.
3. Подключитесь к WB8 по SSH.
4. Запустите:

   ```bash
   bash /root/dmxwb-installer/install.sh
   ```

5. Обновите страницу DMXWB через `Ctrl+F5`.

Успешное обновление заканчивается сообщением:

```text
DMXWB update installation completed.
config_path=/etc/dmxwb/config.json
state_path=/var/lib/dmxwb/state.json
service=enabled_and_started
```

### Проверка обновления

```bash
systemctl is-enabled dmxwb.service
systemctl is-active dmxwb.service
/usr/local/bin/dmxwb --version
test -s /etc/dmxwb/config.json && echo 'Конфигурация сохранена'
if test -f /var/lib/dmxwb/state.json; then echo 'Состояние сохранено'; fi
mosquitto_sub -h 127.0.0.1 -p 1883 -C 1 -t /devices/dmxwb/controls/status
journalctl -u dmxwb.service -n 50 --no-pager
```

Ожидаются `enabled`, `active`, новая версия, сохранённые данные и статус `running`
либо диагностируемый `error`.

## 5. Обычное удаление с сохранением настроек

Обычное удаление убирает программу, службу и Web, но сохраняет конфигурацию и
состояние для последующей повторной установки.

Запустите:

```bash
bash /root/dmxwb-installer/uninstall.sh
```

Успешный результат:

```text
DMXWB application removal completed; config and state were preserved.
config_path=/etc/dmxwb/config.json
state_path=/var/lib/dmxwb/state.json
```

Если `/root/dmxwb-installer` уже удалена, сначала снова скопируйте туда готовую
установочную папку, затем запустите `uninstall.sh`.

### Проверка обычного удаления

```bash
test ! -e /usr/local/bin/dmxwb && echo 'Программа удалена'
test ! -e /etc/systemd/system/dmxwb.service && echo 'Служба удалена'
test ! -e /var/www/dmxwb/index.html && echo 'Web удалён'
test -f /etc/dmxwb/config.json && echo 'Конфигурация сохранена'
if test -f /var/lib/dmxwb/state.json; then echo 'Состояние сохранено'; else echo 'Файл состояния ещё не создавался'; fi
```

## 6. Повторная установка

После обычного удаления запустите ту же установку:

```bash
bash /root/dmxwb-installer/install.sh
```

Скрипт установит приложение и использует сохранённые `config.json` и `state.json`.
Конфигурация, Source и состояние светильников восстановятся.

## 7. Полное удаление вместе с настройками

> **Внимание:** эта команда необратимо удаляет конфигурацию и сохранённое состояние
> DMXWB. Для обычного удаления используйте предыдущий раздел.

Перед полным удалением выключите свет. Затем запустите:

```bash
bash /root/dmxwb-installer/uninstall.sh --purge
```

Удаляются приложение и только два известных пользовательских файла:

```text
/etc/dmxwb/config.json
/var/lib/dmxwb/state.json
```

Другие файлы в каталогах `/etc/dmxwb`, `/var/lib/dmxwb` и `/var/www/dmxwb` не
удаляются.

Успешный результат:

```text
DMXWB application and user data purge completed.
```

### Проверка полного удаления

```bash
test ! -e /usr/local/bin/dmxwb && echo 'Программа удалена'
test ! -e /etc/systemd/system/dmxwb.service && echo 'Служба удалена'
test ! -e /etc/dmxwb/config.json && echo 'Конфигурация удалена'
test ! -e /var/lib/dmxwb/state.json && echo 'Состояние удалено'
test ! -e /var/www/dmxwb/index.html && echo 'Web удалён'
```

## 8. Управление службой и диагностика

```bash
systemctl status dmxwb.service --no-pager -l
systemctl start dmxwb.service
systemctl stop dmxwb.service
systemctl restart dmxwb.service
journalctl -u dmxwb.service -n 100 --no-pager
journalctl -u dmxwb.service -f
```

MQTT, Art-Net, сеть и RS-485 восстанавливаются внутри процесса. После устранения
внешней причины ручной restart обычно не требуется.

## 9. Полный список доступных команд

### Установка и удаление

| Команда | Что делает |
|---|---|
| `bash /root/dmxwb-installer/install.sh` | Устанавливает или обновляет DMXWB, сохраняет config/state, включает и запускает службу. Требует `root`. |
| `bash /root/dmxwb-installer/install.sh --help` | Показывает справку установщика. |
| `bash /root/dmxwb-installer/uninstall.sh` | Удаляет приложение с сохранением config/state. Требует `root`. |
| `bash /root/dmxwb-installer/uninstall.sh --purge` | Необратимо удаляет приложение, config и state. Требует `root`. |
| `bash /root/dmxwb-installer/uninstall.sh --help` | Показывает справку удаления. |

### Служба и журнал

| Команда | Что делает |
|---|---|
| `systemctl start dmxwb.service` | Запускает службу. |
| `systemctl stop dmxwb.service` | Штатно останавливает службу, сохраняет состояние и освобождает DMX-порт. |
| `systemctl restart dmxwb.service` | Штатно перезапускает процесс. |
| `systemctl status dmxwb.service --no-pager -l` | Показывает состояние и последние сообщения. |
| `systemctl is-active dmxwb.service` | Возвращает `active`, если служба работает. |
| `systemctl is-enabled dmxwb.service` | Возвращает `enabled`, если включён автозапуск. |
| `systemctl enable dmxwb.service` | Включает автозапуск. Установщик уже делает это. |
| `systemctl disable dmxwb.service` | Отключает автозапуск без удаления файлов. |
| `journalctl -u dmxwb.service -n 100 --no-pager` | Показывает последние 100 событий. |
| `journalctl -u dmxwb.service -f` | Показывает новые события до `Ctrl+C`. |

### Программа DMXWB

| Команда | Что делает |
|---|---|
| `/usr/local/bin/dmxwb --version` | Показывает версию. |
| `/usr/local/bin/dmxwb --help` | Показывает параметры программы. |
| `/usr/local/bin/dmxwb` | Запускает DMXWB в foreground. Не запускать одновременно со службой. |
| `/usr/local/bin/dmxwb --config PATH --state PATH` | Диагностический foreground-запуск с явно заданными разными файлами. |

### MQTT-команды

Все команды публикуются без retain.

| Назначение | Topic | Payload |
|---|---|---|
| Выбор Source | `/devices/dmxwb/controls/source/on` | `mqtt` или `artnet` |
| Имя Fixture | `/devices/dmxwb_fixture_<id>/controls/name/on` | UTF-8 имя |
| Power Fixture | `/devices/dmxwb_fixture_<id>/controls/power/on` | `0` или `1` |
| R/G/B Fixture | `/devices/dmxwb_fixture_<id>/controls/red/on`, `green/on`, `blue/on` | `0..255` |
| Color Fixture | `/devices/dmxwb_fixture_<id>/controls/color/on` | `R;G;B` |
| Brightness Fixture | `/devices/dmxwb_fixture_<id>/controls/brightness/on` | `0..100` |
| Temperature Fixture | `/devices/dmxwb_fixture_<id>/controls/temperature/on` | `0..100` |
| Reset Fixture | `/devices/dmxwb_fixture_<id>/controls/reset/on` | `1` |
| Команды Group | `/devices/dmxwb_group_<id>/controls/<control>/on` | Те же name, power, R/G/B, color, brightness, temperature и reset |
| Имя Scene | `/devices/dmxwb_scene_<id>/controls/name/on` | UTF-8 имя |
| Apply Scene | `/devices/dmxwb_scene_<id>/controls/apply/on` | `1` |
| Config Set | `/dmxwb/config/set` | JSON: `request_id`, `expected_revision`, полная `config` |
| Create Scene | `/dmxwb/scenes/create` | JSON: `request_id`, `name` |
| Rename Scene | `/dmxwb/scenes/<id>/rename` | JSON: `request_id`, `name` |
| Apply Scene | `/dmxwb/scenes/<id>/apply` | JSON: `request_id` |
| Overwrite Scene | `/dmxwb/scenes/<id>/overwrite` | JSON: `request_id` |
| Delete Scene | `/dmxwb/scenes/<id>/delete` | JSON: `request_id` |

Для создания и удаления Fixture/Group/Scene используйте Web DMXWB: он
проверяет значения, revision и подтверждение результата. После сохранения
созданные Fixture/Group/Scene и их команды видны также в штатном интерфейсе
Wiren Board. Структурное создание и удаление в штатном WB UI не выполняется.
