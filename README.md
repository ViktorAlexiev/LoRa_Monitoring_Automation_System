LoRa Monitoring & Automation System

Разпределена система за мониторинг и автоматизирано управление на поливането на земеделско
предприятие. Нископотребяващи безжични модули (сензорни, изпълнителни, повторители) комуникират
по LoRa на батерия/соларно захранване; LoRa gateway препраща трафика по WiFi/MQTT към централен
сървър (Raspberry Pi), който взема решения за поливане и предоставя уеб dashboard за наблюдение и
управление в реално време.

Пълната техническа документация (протокол, схема на базата данни, каталог на грешките, admin
панел) е в [`documentation/documentation.docx`](documentation/documentation.docx). Архитектурата
не е обвързана с конкретна индустрия — приложима е навсякъде, където трябва да се следят
разпределени физически точки и да се управляват изпълнителни устройства от разстояние.

## Архитектура накратко

```
Sensor/Executor/Repeater (LoRa, батерия+соларно)
          │  AES-128 + MAC + anti-replay
          ▼
     LoRa Gateway (TTGO)
          │  WiFi / MQTT
          ▼
   mosquitto  →  mqtt_bridge.py ──┐
                                   ├─► MySQL/SQLite
   reconciler.py  ◄────────────────┤
   desired_state_setter.py ◄───────┤
   health_checker.py ◄─────────────┘
          │
          ▼
   web-api (FastAPI)  ◄──►  dashboard (React, само четене на бизнес логика)
```

- **`reconciler.py`** — единственият процес, който пише `current_state`/`desired_state`/командите;
  свежда разминаването между желано и текущо състояние към реалност през MQTT.
- **`desired_state_setter.py`** — решава `desired_state` за зони в режим clock/threshold.
- **`health_checker.py`** — само чете и пише в `zone_errors` (пълен каталог от грешки — виж
  документацията).
- **`mqtt_bridge.py`** — пише единствено телеметрия (показания, heartbeat).

Всеки скрипт пише само в собствените си колони/таблици — няма конфликти при паралелна работа.

## Структура на репото

| Папка | Съдържание |
|---|---|
| [`web_dashboard/backend`](web_dashboard/backend) | FastAPI приложение + 4-те Python демона (`daemons/`) |
| [`web_dashboard/frontend`](web_dashboard/frontend) | React dashboard (Vite) |
| [`firmware_uploader`](firmware_uploader) | Desktop инструмент (PyInstaller) за флашване на фърмуера и присвояване на module ID |
| [`firmware_uploader/firmware_src`](firmware_uploader/firmware_src) | Firmware source (sensor / executor / repeater / gateway, Arduino) |
| [`deploy`](deploy) | `docker-compose.yml` + инструкции за реален деплой на Raspberry Pi |
| [`documentation`](documentation) | Пълна техническа документация (Word) |

## Локално стартиране (dev, SQLite, без Docker)

```bash
# Backend
cd web_dashboard/backend
python -m venv venv && venv\Scripts\activate      # Windows
pip install -r requirements.txt
uvicorn app.main:app --reload --port 8000          # SEED_DEMO_DATA=true по подразбиране

# Frontend
cd web_dashboard/frontend
npm install
npm run dev                                         # http://localhost:5173
```

Демо акаунти след seed: `ivo` / `agronom` / `viewer`, парола `123` за всички.

### Демоните (по избор, за пълен цикъл клапан→помпа)

Изискват реален или symuliран MQTT broker (виж `daemons/system_config.py` — по подразбиране
`localhost:1883`, anonymous):

```bash
python daemons/reconciler.py
python daemons/desired_state_setter.py
python daemons/health_checker.py
python daemons/mqtt_bridge.py
```

## Реален деплой (Raspberry Pi, Docker)

Виж [`deploy/README.md`](deploy/README.md) за пълните стъпки (build на ARM64 images, копиране на
`deploy/` през SSH, `docker compose up -d`, auto-start след токов удар, планирани стъпки за
HTTPS/MQTT TLS/backup).

## Firmware / firmware_uploader

`firmware_uploader/` е отделен desktop инструмент за флашване на фърмуера на физическите модули
(sensor/executor/repeater/gateway) и присвояване на техните module ID — същите ID-та, които после
се въвеждат в admin панела на dashboard-а. Изисква `firmware_uploader/tools/` (arduino-cli,
avrdude, esptool — вече включени в репото) и `firmware_uploader/network_key.py`, генериран локално
(виж по-долу).

## Сигурност — преди да работиш с репото

**Тия файлове НЕ се качват в git** (виж [`.gitignore`](.gitignore)) и трябва да се създадат/копират
локално ръчно, никога от git history:

- **`deploy/.env`** — MySQL/MQTT credentials + hidden admin парола за реалния деплой. Генерирай
  дълги случайни стойности; никога не ги пиши на ръка.
- **`deploy/mosquitto_passwd`** — генерира се с `mosquitto_passwd -b -c mosquitto_passwd <user> <парола>`
  (специфичен PBKDF2 формат, никога hand-written).
- **`firmware_uploader/network_key.py`** — единственият AES-128 мрежов ключ, споделен от ВСИЧКИ
  устройства в радио мрежата. Ако изтече, цялата радио комуникация на обекта е компрометирана.
  Формат:
  ```python
  NETWORK_KEY_HEX = "32 hex символа тук (16 bytes)"
  ```
  Ако ключът някога трябва да се ротира — смени стойността тук и преконфигурирай (Update/Upload)
  ВСИЧКИ устройства в мрежата, иначе спират да се разбират помежду си.

Пълният преглед на съзнателно приетите рискове в радио протокола (единен споделен ключ, липса на
допълнителна anti-jamming защита и т.н.) е в
[`documentation/Security_Accepted_Risks.docx`](documentation/Security_Accepted_Risks.docx).

## Известни открити точки (не са бъгове, а оставащ обхват)

- HTTPS на dashboard-а и MQTT TLS — съзнателно отложени, чист LAN трафик засега.
- Backup на базата (systemd timer + `mysqldump`) — описан концептуално в документацията, предстои
  реална имплементация на обекта.
- MQTT ACL per-device, Pi OS hardening (fail2ban, unattended-upgrades) — виж
  [`deploy/README.md`](deploy/README.md), раздел "Сигурност".
