#!/usr/bin/env bash
# Компилира и пуска всички нативни (C++) тестове на РЕАЛНИТЕ .cpp файлове от
# firmware_src/, срещу верифицирана AES-128 (aes_selfcheck) и мокнати Arduino/EEPROM/
# Wire/LoRa библиотеки. Спира на първата провалена компилация или тест (`set -e`).
set -e
cd "$(dirname "$0")"

CXX="g++ -std=c++17 -O0 -g -Wall"
FAKE="-I fakelibs"
SRC=../../firmware_src

run() {   # run <заглавие> <exe> <включвания -I> <файлове...>
  local title="$1"; local exe="$2"; local inc="$3"; shift 3
  echo
  echo "=== $title ==="
  $CXX $FAKE $inc -o "$exe" "$@"
  "./$exe"
}

echo "=== AES-128 self-check (FIPS-197) ==="
$CXX -o aes_selfcheck.exe aes_selfcheck.cpp
./aes_selfcheck.exe

run "crypto_common (sensor - идентичен и в executor/repeater/gateway)" test_crypto_common.exe "-I $SRC/sensor" \
  test_crypto_common.cpp fakelibs/arduino_mock.cpp $SRC/sensor/crypto_common.cpp

run "ceiling_counter (sensor)" test_ceiling_counter.exe "-I $SRC/sensor" \
  test_ceiling_counter.cpp fakelibs/arduino_mock.cpp fakelibs/eeprom_mock.cpp $SRC/sensor/ceiling_counter.cpp

run "executor/queues" test_queues.exe "-I $SRC/executor" \
  test_queues.cpp fakelibs/arduino_mock.cpp $SRC/executor/queues.cpp

run "executor/consumers" test_consumers.exe "-I $SRC/executor" \
  test_consumers.cpp fakelibs/arduino_mock.cpp $SRC/executor/consumers.cpp

run "repeater/dedup" test_dedup.exe "-I $SRC/repeater" \
  test_dedup.cpp fakelibs/arduino_mock.cpp $SRC/repeater/dedup.cpp

run "executor/config_storage (pinFromString, loadConfigFromEeprom, SF/BW)" test_config_storage_executor.exe "-I $SRC/executor" \
  test_config_storage_executor.cpp fakelibs/arduino_mock.cpp fakelibs/eeprom_mock.cpp \
  $SRC/executor/config_storage.cpp $SRC/executor/ceiling_counter.cpp $SRC/executor/radio_timing.cpp

run "sensor/config_storage (вкл. SF/BW)" test_config_storage_sensor.exe "-I $SRC/sensor" \
  test_config_storage_sensor.cpp fakelibs/arduino_mock.cpp fakelibs/eeprom_mock.cpp \
  $SRC/sensor/config_storage.cpp $SRC/sensor/ceiling_counter.cpp $SRC/sensor/radio_timing.cpp

run "repeater/config_storage (вкл. SF/BW)" test_config_storage_repeater.exe "-I $SRC/repeater" \
  test_config_storage_repeater.cpp fakelibs/arduino_mock.cpp fakelibs/eeprom_mock.cpp \
  $SRC/repeater/config_storage.cpp $SRC/repeater/ceiling_counter.cpp $SRC/repeater/radio_timing.cpp

run "sensor/sensors_io (SHT21 MSB/LSB fix + диапазонна проверка)" test_sensors_io.exe "-I $SRC/sensor" \
  test_sensors_io.cpp fakelibs/arduino_mock.cpp fakelibs/wire_mock.cpp $SRC/sensor/sensors_io.cpp

run "radio_timing (ToA/LDRO/CAD/ACK timeout-и от SF/BW)" test_radio_timing.exe "-I $SRC/sensor" \
  test_radio_timing.cpp fakelibs/arduino_mock.cpp $SRC/sensor/radio_timing.cpp

run "channel_access (неблокиращ CAD: сондиране, backoff, срок)" test_channel_access.exe "-I $SRC/sensor" \
  test_channel_access.cpp fakelibs/arduino_mock.cpp $SRC/sensor/radio_timing.cpp $SRC/sensor/channel_access.cpp

run "length-dispatch логика (Gateway/Executor, без числени колизии)" test_length_dispatch.exe "-I $SRC/sensor" \
  test_length_dispatch.cpp fakelibs/arduino_mock.cpp

echo
echo "=========================================="
echo "ВСИЧКИ НАТИВНИ (C++) ТЕСТОВЕ МИНАХА"
echo "=========================================="
