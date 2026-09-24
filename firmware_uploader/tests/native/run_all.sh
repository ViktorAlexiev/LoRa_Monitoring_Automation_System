#!/usr/bin/env bash
# Компилира и пуска всички нативни (C++) тестове на РЕАЛНИТЕ .cpp файлове от
# firmware_src/, срещу верифицирана AES-128 (aes_selfcheck) и мокнати Arduino/EEPROM/
# Wire библиотеки. Спира на първата провалена компилация или тест (`set -e`).
set -e
cd "$(dirname "$0")"

CXX="g++ -std=c++17 -O0 -g -Wall"
FAKE="-I fakelibs"

echo "=== AES-128 self-check (FIPS-197) ==="
$CXX -o aes_selfcheck.exe aes_selfcheck.cpp
./aes_selfcheck.exe

echo
echo "=== crypto_common (sensor - идентичен и в executor/repeater/gateway) ==="
$CXX $FAKE -I ../../firmware_src/sensor -o test_crypto_common.exe \
  test_crypto_common.cpp fakelibs/arduino_mock.cpp ../../firmware_src/sensor/crypto_common.cpp
./test_crypto_common.exe

echo
echo "=== ceiling_counter (sensor) ==="
$CXX $FAKE -I ../../firmware_src/sensor -o test_ceiling_counter.exe \
  test_ceiling_counter.cpp fakelibs/arduino_mock.cpp fakelibs/eeprom_mock.cpp \
  ../../firmware_src/sensor/ceiling_counter.cpp
./test_ceiling_counter.exe

echo
echo "=== executor/queues ==="
$CXX $FAKE -I ../../firmware_src/executor -o test_queues.exe \
  test_queues.cpp fakelibs/arduino_mock.cpp ../../firmware_src/executor/queues.cpp
./test_queues.exe

echo
echo "=== executor/consumers ==="
$CXX $FAKE -I ../../firmware_src/executor -o test_consumers.exe \
  test_consumers.cpp fakelibs/arduino_mock.cpp ../../firmware_src/executor/consumers.cpp
./test_consumers.exe

echo
echo "=== repeater/dedup ==="
$CXX $FAKE -I ../../firmware_src/repeater -o test_dedup.exe \
  test_dedup.cpp fakelibs/arduino_mock.cpp ../../firmware_src/repeater/dedup.cpp
./test_dedup.exe

echo
echo "=== executor/config_storage (pinFromString + loadConfigFromEeprom) ==="
$CXX $FAKE -I ../../firmware_src/executor -o test_config_storage_executor.exe \
  test_config_storage_executor.cpp fakelibs/arduino_mock.cpp fakelibs/eeprom_mock.cpp \
  ../../firmware_src/executor/config_storage.cpp ../../firmware_src/executor/ceiling_counter.cpp
./test_config_storage_executor.exe

echo
echo "=== sensor/config_storage ==="
$CXX $FAKE -I ../../firmware_src/sensor -o test_config_storage_sensor.exe \
  test_config_storage_sensor.cpp fakelibs/arduino_mock.cpp fakelibs/eeprom_mock.cpp \
  ../../firmware_src/sensor/config_storage.cpp ../../firmware_src/sensor/ceiling_counter.cpp
./test_config_storage_sensor.exe

echo
echo "=== repeater/config_storage ==="
$CXX $FAKE -I ../../firmware_src/repeater -o test_config_storage_repeater.exe \
  test_config_storage_repeater.cpp fakelibs/arduino_mock.cpp fakelibs/eeprom_mock.cpp \
  ../../firmware_src/repeater/config_storage.cpp ../../firmware_src/repeater/ceiling_counter.cpp
./test_config_storage_repeater.exe

echo
echo "=== sensor/sensors_io (SHT21 MSB/LSB fix + диапазонна проверка) ==="
$CXX $FAKE -I ../../firmware_src/sensor -o test_sensors_io.exe \
  test_sensors_io.cpp fakelibs/arduino_mock.cpp fakelibs/wire_mock.cpp \
  ../../firmware_src/sensor/sensors_io.cpp
./test_sensors_io.exe

echo
echo "=== length-dispatch логика (Gateway/Executor, без числени колизии) ==="
$CXX $FAKE -I ../../firmware_src/sensor -o test_length_dispatch.exe \
  test_length_dispatch.cpp fakelibs/arduino_mock.cpp
./test_length_dispatch.exe

echo
echo "=========================================="
echo "ВСИЧКИ НАТИВНИ (C++) ТЕСТОВЕ МИНАХА"
echo "=========================================="
