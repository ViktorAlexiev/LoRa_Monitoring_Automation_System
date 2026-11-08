#!/usr/bin/env bash
# Нативни тестове на firmware_src/ESP32_PCB(temp_hum) - РЕАЛНИТЕ .cpp/.ino файлове, срещу фалшиви
# Arduino/ESP-IDF/NVS/WiFi/PubSubClient/LoRa (fakelibs_esp32/) и реалната ArduinoJson.
# Спира на първата провалена компилация или тест. Без реален ESP32/радио - виж README за границите.
#
#   cd tests/native/esp32 && ./run_esp32.sh
#   ESP=<друга папка с firmware> ./run_esp32.sh      # напр. за мутационни проверки
set -e
cd "$(dirname "$0")"

ESP=${ESP:-"../../../firmware_src/ESP32_PCB(temp_hum)"}
AJ=${ARDUINOJSON:-"D:/A1_WinData/Documents/Arduino/libraries/ArduinoJson/src"}
PY=${PYTHON:-python}
# -static: на тази машина в PATH има няколко libstdc++-6.dll (Git / MinGW) и динамичното свързване
# води до срив в iostream; статичното е надеждно.
CXX="g++ -std=c++17 -O0 -g -static -Wall -Wno-unknown-pragmas -Wno-deprecated-declarations -Wno-unused-function"
FAKE="-I fakelibs_esp32 -I ../fakelibs"
M=fakelibs_esp32/mocks_esp32.cpp

echo "=== 1. Общите файлове са еднакви във всички скечове ==="
for f in crypto_common.cpp crypto_common.h radio_timing.cpp radio_timing.h channel_access.cpp channel_access.h cad.cpp cad.h; do
  for d in executor repeater gateway; do
    cmp -s "$ESP/sensor/$f" "$ESP/$d/$f" || { echo "РАЗЛИКА: sensor/$f != $d/$f"; exit 1; }
  done
done
for f in ceiling_counter.cpp ceiling_counter.h; do
  for d in executor repeater; do cmp -s "$ESP/sensor/$f" "$ESP/$d/$f" || { echo "РАЗЛИКА: sensor/$f != $d/$f"; exit 1; }; done
done
for f in power_mgmt.cpp power_mgmt.h; do cmp -s "$ESP/executor/$f" "$ESP/repeater/$f" || { echo "РАЗЛИКА: executor/$f != repeater/$f"; exit 1; }; done
for d in executor repeater gateway; do cmp -s "$ESP/sensor/board_pins.h" "$ESP/$d/board_pins.h" || { echo "РАЗЛИКА: board_pins.h sensor != $d"; exit 1; }; done
echo "OK"

run() {   # run <заглавие> <exe> <включвания -I> <файлове...>
  local title="$1"; local exe="$2"; local inc="$3"; shift 3
  echo; echo "=== $title ==="
  $CXX $FAKE $inc -o "$exe" "$@"
  "./$exe"
}

run "2. ceiling_counter (NVS + RTC)" test_esp32_ceiling.exe "-I $ESP/sensor" \
  test_esp32_ceiling.cpp $M "$ESP/sensor/ceiling_counter.cpp"

run "3. sensor/config_storage (NVS)" test_esp32_config_sensor.exe "-I $ESP/sensor" \
  test_esp32_config_sensor.cpp $M "$ESP/sensor/config_storage.cpp" "$ESP/sensor/ceiling_counter.cpp" "$ESP/sensor/radio_timing.cpp"
run "4. executor/config_storage (NVS)" test_esp32_config_executor.exe "-I $ESP/executor" \
  test_esp32_config_executor.cpp $M "$ESP/executor/config_storage.cpp" "$ESP/executor/ceiling_counter.cpp" "$ESP/executor/radio_timing.cpp"
run "5. repeater/config_storage (NVS)" test_esp32_config_repeater.exe "-I $ESP/repeater" \
  test_esp32_config_repeater.cpp $M "$ESP/repeater/config_storage.cpp" "$ESP/repeater/ceiling_counter.cpp" "$ESP/repeater/radio_timing.cpp"
run "6. executor_wifi/config_storage (NVS)" test_esp32_config_executor_wifi.exe "-I $ESP/executor_wifi" \
  test_esp32_config_executor_wifi.cpp $M "$ESP/executor_wifi/config_storage.cpp"

run "7. power_mgmt - лек сън (executor)" test_esp32_power_executor.exe "-I $ESP/executor" \
  test_esp32_power_lightsleep.cpp $M "$ESP/executor/power_mgmt.cpp"
run "8. power_mgmt - дълбок сън (sensor)" test_esp32_power_sensor.exe "-I $ESP/sensor" \
  test_esp32_power_deepsleep.cpp $M "$ESP/sensor/power_mgmt.cpp"

run "9. config_esp32.ino - CFG парсер (stage 1)" test_esp32_cfg_parser.exe "-I $ESP" \
  test_esp32_cfg_parser.cpp $M

run "10. executor_wifi (реален .ino + mqtt_exec.cpp, фалшив брокер)" test_executor_wifi.exe "-I $AJ -I $ESP -I $ESP/executor_wifi" \
  test_executor_wifi.cpp $M "$ESP/executor_wifi/mqtt_exec.cpp" "$ESP/executor_wifi/config_storage.cpp"

echo; echo "=== 11. End-to-end: sensor.ino -> gateway.ino -> MQTT (Python + два нативни драйвера) ==="
$CXX $FAKE -I "$AJ" -I "$ESP" -I "$ESP/sensor" -o sensor_capture.exe sensor_capture.cpp $M ../fakelibs/wire_mock.cpp \
  "$ESP/sensor/config_storage.cpp" "$ESP/sensor/ceiling_counter.cpp" "$ESP/sensor/crypto_common.cpp" \
  "$ESP/sensor/radio_timing.cpp" "$ESP/sensor/channel_access.cpp" "$ESP/sensor/cad.cpp" "$ESP/sensor/radio_tx.cpp" \
  "$ESP/sensor/sensors_io.cpp" "$ESP/sensor/power_mgmt.cpp"
$CXX $FAKE -I "$AJ" -I "$ESP" -I "$ESP/gateway" -o gateway_rx.exe gateway_rx.cpp $M \
  "$ESP/gateway/config_storage.cpp" "$ESP/gateway/crypto_common.cpp" "$ESP/gateway/radio_timing.cpp" \
  "$ESP/gateway/channel_access.cpp" "$ESP/gateway/cad.cpp" "$ESP/gateway/lora_handlers.cpp" "$ESP/gateway/wifi_mqtt.cpp"
$CXX $FAKE -I "$AJ" -I "$ESP" -I "$ESP/executor" -o executor_rx.exe executor_rx.cpp $M \
  "$ESP/executor/config_storage.cpp" "$ESP/executor/ceiling_counter.cpp" "$ESP/executor/crypto_common.cpp" \
  "$ESP/executor/radio_timing.cpp" "$ESP/executor/channel_access.cpp" "$ESP/executor/cad.cpp" \
  "$ESP/executor/radio_io.cpp" "$ESP/executor/consumers.cpp" "$ESP/executor/queues.cpp" "$ESP/executor/power_mgmt.cpp"
$CXX $FAKE -I "$AJ" -I "$ESP" -I "$ESP/repeater" -o repeater_rx.exe repeater_rx.cpp $M \
  "$ESP/repeater/config_storage.cpp" "$ESP/repeater/ceiling_counter.cpp" "$ESP/repeater/crypto_common.cpp" \
  "$ESP/repeater/radio_timing.cpp" "$ESP/repeater/channel_access.cpp" "$ESP/repeater/cad.cpp" \
  "$ESP/repeater/radio_io.cpp" "$ESP/repeater/dedup.cpp" "$ESP/repeater/power_mgmt.cpp"
PYTHONIOENCODING=utf8 $PY test_e2e.py

echo; echo "=== 12. LoRa executor (реален executor.ino, фалшиво радио/сън) ==="
PYTHONIOENCODING=utf8 $PY test_executor_lora.py

echo; echo "=== 13. Repeater (реален repeater.ino; входът са истински sensor пакети) ==="
PYTHONIOENCODING=utf8 $PY test_repeater.py

echo
echo "=========================================="
echo "ВСИЧКИ ESP32 ТЕСТОВЕ МИНАХА"
echo "=========================================="
