#!/usr/bin/env bash
# Нативни тестове на ESP-IDF прошивките (firmware_src/ESP32_PCB(temp_hum)_ESPIDF).
# Компилира РЕАЛНИТЕ components/ и apps/<приложение>/main, РЕАЛНИЯ RadioLib (с неговия EspHal) и cJSON срещу заместителите
# в fake_idf/ (симулатор на FreeRTOS във виртуално време, модел на SX1262 върху SPI командите, NVS, GPIO, I2C, Wi-Fi,
# MQTT брокер, mbedtls). Общите настройки са в common.sh.
#
#   cd tests/native/espidf && ./run_espidf.sh [тест ...]       (без аргументи: всички)
#   FW=<път до прошивката> OUT=<папка за .exe> ./run_espidf.sh  (за мутационни проверки)
set -e
cd "$(dirname "$0")"
. ./common.sh

INC="$INC_BASE"
declare -A SRC EXTRA
SRC[rtos]="test_rtos.cpp $SIM"
SRC[core]="test_core.cpp $SIM $HAL $CORE $CFG"
SRC[radio]="test_radio.cpp $SIM $HAL $CORE $RADIO"
SRC[net]="test_net.cpp $SIM $HAL $CORE $NET"
SRC[sensor]="test_sensor.cpp $SIM $HAL $CORE $RADIO $(app_srcs sensor)"
EXTRA[sensor]="-I $A/sensor/main"
SRC[executor]="test_executor.cpp $SIM $HAL $CORE $RADIO $(app_srcs executor)"
EXTRA[executor]="-I $A/executor/main"
SRC[repeater]="test_repeater.cpp $SIM $HAL $CORE $RADIO $(app_srcs repeater)"
EXTRA[repeater]="-I $A/repeater/main"
SRC[gateway]="test_gateway.cpp $SIM $HAL $CORE $RADIO $NET $(app_srcs gateway)"
EXTRA[gateway]="-I $A/gateway/main"
SRC[executor_wifi]="test_executor_wifi.cpp $SIM $HAL $CORE $NET $(app_srcs executor_wifi)"
EXTRA[executor_wifi]="-I $A/executor_wifi/main"
SRC[config]="test_config_app.cpp $SIM $HAL $CORE $CFG $(app_srcs config)"
EXTRA[config]="-I $A/config/main"

ALL="rtos core radio net sensor executor repeater gateway executor_wifi config"
TARGETS=${@:-$ALL}
for t in $TARGETS; do
  if [ -z "${SRC[$t]}" ]; then echo "непознат тест: $t"; exit 2; fi
  if [ ! -f "${SRC[$t]%% *}" ]; then echo "(пропускам $t - няма ${SRC[$t]%% *})"; continue; fi
  echo; echo "=== $t ==="
  rm -f "$OUT/$t.exe"
  if ! $CXX $INC ${EXTRA[$t]} ${SRC[$t]} -o "$OUT/$t.exe" > "$OUT/$t.log" 2>&1; then
    grep -v "pragma message\|^ *[0-9]* | #pragma\|In file included\|^ *from " "$OUT/$t.log" | head -60
    echo "КОМПИЛАЦИЯТА НА $t СЕ ПРОВАЛИ"; exit 1
  fi
  grep "warning" "$OUT/$t.log" | grep -v "pragma message" | head -20 || true
  "$OUT/$t.exe"
done
echo
echo "=========================================="
echo "ВСИЧКИ ESP-IDF ТЕСТОВЕ МИНАХА: $TARGETS"
echo "=========================================="
