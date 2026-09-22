# Деплой на web_dashboard - реални стъпки

Файловете в тая папка (`docker-compose.yml`, `.env`, `mosquitto.conf`, `mosquitto_passwd`) са
всичко, което трябва да е на самия Raspberry Pi. Кодът (`backend/`, `frontend/`) се build-ва
отделно и се пуска в private registry - на Pi-то никога не сяда изходен код, само готови images.

## 1. Еднократно (веднъж изобщо, на лаптопа)

```bash
docker login ghcr.io -u твоя-github-username
```

## 2. При всяка нова версия за пращане (от лаптопа, от корена на repo-то)

```bash
cd web_dashboard/backend
docker buildx build --platform linux/arm64 -t ghcr.io/твоя-username/agromonitor-backend:latest --push .

cd ../frontend
docker buildx build --platform linux/arm64 -t ghcr.io/твоя-username/agromonitor-frontend:latest --push .
```

(Забележка: `npm run build` НЕ се пуска отделно - `frontend/Dockerfile` е multi-stage и го прави
сам вътре в build-а.)

## 3. Първа инсталация на чисто нов Pi

На самото Pi, еднократно:
```bash
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker $USER   # после излез/влез пак
```

Копирай цялата тая папка (`deploy/`) на Pi-то, напр. в `/opt/agromonitor/`, после:
```bash
cd /opt/agromonitor
docker login ghcr.io -u твоя-github-username
docker compose up -d
```

Docker сам тегли `mysql`/`mosquitto` (стандартни), тегли твоите два private images (arm64
вариант автоматично), създава базата **празна** (само скрития admin акаунт от `.env` - виж
`SEED_DEMO_DATA: "false"` в `docker-compose.yml`), вдига всичко.

## 4. Ъпдейт на вече работещ Pi

```bash
cd /opt/agromonitor
docker compose pull
docker compose up -d
```
Не пипа `mysql_data` volume-а.

---

## Креденшъли

Реалните стойности живеят само в `.env` (никога в тоя README или в git - виж бележката в
началото на `.env`), затова тук не са изписани буквално:

- **MySQL** (`MYSQL_USER`/`MYSQL_PASSWORD`/`MYSQL_ROOT_PASSWORD`) - дълга случайна парола е ОК,
  никой не я пише на ръка.
- **MQTT** (`MQTT_USERNAME`/`MQTT_PASSWORD`) - виж `mosquitto_passwd` (генериран с `mosquitto_passwd
  -b -c mosquitto_passwd <user> <парола>`, никога hand-write-ван - това е специфичен PBKDF2 формат).
- **Скрит системен admin в dashboard-а** (`SYSTEM_ADMIN_USERNAME`/`SYSTEM_ADMIN_PASSWORD`) -
  създава се автоматично при първо стартиране на web-api от тия env-и (виж `app/main.py`).

За смяна на парола (напр. компрометиран акаунт): редактирай `.env`, после `docker compose up -d`
(само пипнатите контейнери се пресъздават - MySQL данните не се губят). MySQL/MQTT паролите не
се пипат сами - смяна там изисква и промяна на съответния потребител вътре в MySQL/mosquitto, не
само в `.env`.

## Съзнателно решение: без HTTPS засега

`nginx.conf` е чист http (порт 80) - нарочно, не пропуск. `COOKIE_SECURE=false` в
`docker-compose.yml` съответства на това (иначе браузърът мълчаливо отказва да пази session
cookie-то по http - точно тоя бъг се хвана веднъж на телефон, вече е оправен чрез тая настройка).
Ако по-късно решиш да добавиш HTTPS на даден обект (certbot ако има публичен domain, или
self-signed/локален CA ако е чисто LAN) - виж разговора с Claude по темата за конкретните стъпки;
не забравяй да върнеш `COOKIE_SECURE` на `true` тогава.

Същото важи и за **MQTT TLS** - `mosquitto.conf` слуша чист (некриптиран) `1883`, с username/
парола, но без TLS. Отложено за по-късно по същата логика - връзката е само в LAN-а на фермата
(gateway → Pi), не пътува по интернет. Ако някога се наложи TLS на MQTT, добавя се отделен
`listener 8883` с `cafile`/`certfile`/`keyfile` в `mosquitto.conf`, плюс сертификат - независимо
решение от HTTPS-а на самия dashboard, могат да се добавят по различно време.

## Друго за наблюдение

- `docker-compose.yml`-ът в тая папка има `ports: 1883:1883` за mosquitto - това е нарочно
  (гейтуеят е физическо устройство на LAN-а, трябва да го достигне), но никога не forward-вай
  тоя порт на рутера навън.

## Auto-start след рестарт / токов удар

Две отделни неща трябва да са верни, за да се вдигне системата сама след прекъсване на тока -
и двете се проверяват еднократно, не при всеки reboot:

**1. Самият Docker daemon трябва да стартира при boot.** Инсталационният скрипт
(`get-docker.sh`, стъпка 3 по-горе) обикновено го прави сам, но провери изрично:
```bash
systemctl is-enabled docker   # трябва да покаже "enabled"
```
Ако покаже `disabled`:
```bash
sudo systemctl enable docker
```

**2. Всеки контейнер вече има `restart: unless-stopped`** (виж `docker-compose.yml`) - това е
достатъчно само по себе си: Docker помни това намерение trайно (не само за текущата сесия), и
щом daemon-ът тръгне (т.е. веднага щом (1) е вярно), сам рестартира всички контейнери от тоя
compose проект - **не е нужно** да пускаш `docker compose up -d` ръчно след всеки reboot.
`unless-stopped` (за разлика от `always`) уважава и ръчно спиране - ако ти самият спреш нещо с
`docker compose stop`, то остава спряно и след reboot, докато не го вдигнеш пак изрично.

**Как да тестваш реално, не само на теория** (на самото Pi, при планирана поддръжка - не се
поддава на токов удар "на живо", разбира се):
```bash
sudo reboot
# изчакай ~1-2 мин, после:
docker compose -f /opt/agromonitor/docker-compose.yml ps
```
Всички 8 контейнера трябва да са `Up`/`healthy` без нито една ръчна команда между reboot-а и
проверката.

## Сигурност

**Вече направено** (виж git history / разговора с Claude по темата):
- Силни, случайни пароли за MySQL/MQTT + конкретни admin креденшъли (вместо старите демо `123`).
- Login lockout - 5 грешни опита заключват акаунта за 15 мин (виж `app/auth.py`).
- Всичките 5 backend контейнера (web-api + 4 демона) вече текат като обикновен потребител
  (`appuser`), не root - виж `backend/Dockerfile`.
- `nginx` контейнерът също - `nginxinc/nginx-unprivileged` вместо стандартния `nginx:alpine`
  (последният слуша порт 80 отвътре само като root).
- Лимит на логовете на всеки контейнер (`x-logging` в `docker-compose.yml`, 10m x 3 файла) - без
  него `json-file` драйверът расте неограничено и може бавно да напълни SD картата с месеци
  uptime.

**Съзнателно отложено засега** (виж по-горе, "Съзнателно решение: без HTTPS засега") - HTTPS на
dashboard-а, TLS на MQTT.

**Още неадресирано - следващи стъпки, когато обектът стане реален (не само bench test)**:
- **MQTT ACL по topic** - в момента един-единствен MQTT потребител (`admin`) може да publish-ва
  на всеки topic. Ако някой изпълнител/сензор бъде компрометиран физически (кражба на устройство
  с изгубени credentials), той може технически да праща команди за произволна зона, не само
  своята. Mosquitto поддържа ACL файл (`acl_file` в `mosquitto.conf`) с отделен потребител per
  device - по-голяма задача, не за днес.
- **Pi OS хардениране** (Raspberry Pi OS, извън самия Docker):
  - Смени паролата на потребителя `pi` (или изобщо го премахни, ако вече имаш друг sudo user) -
    default credentials на Pi е първото нещо, което автоматизиран скенер пробва.
  - SSH само с ключ, изключи password auth (`PasswordAuthentication no` в
    `/etc/ssh/sshd_config`), после `sudo systemctl restart ssh`.
  - Firewall (`sudo apt install ufw`): разреши само SSH + 80 (dashboard) + 1883 (MQTT, само ако
    гейтуеят наистина е на друг физически сегмент от same-Pi setup) от LAN-а, всичко друго deny.
    ```bash
    sudo ufw allow from <твоята LAN подмрежа> to any port 22,80,1883
    sudo ufw enable
    ```
  - Автоматични security ъпдейти на самата ОС (`sudo apt install unattended-upgrades`) - Docker
    images се ъпдейтват ръчно (стъпка 4 по-горе), но самата Raspberry Pi OS/ядрото трябва да са
    патчнати и без твоя намеса.
  - `fail2ban` за SSH, ако портът изобщо е достижим извън чист LAN (обикновено не би трябвало да
    е - виж бележката за router forwarding по-горе).
- **MySQL/mosquitto контейнерите** сами по себе си вървят като root вътре (официални images,
  не са пипани) - по-нисък приоритет от backend/frontend, защото не изпълняват никакъв наш код,
  само стандартния сървърен процес на самия image.
