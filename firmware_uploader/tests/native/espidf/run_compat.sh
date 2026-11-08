#!/usr/bin/env bash
# Диференциален тест на съвместимост: ESP-IDF прошивките срещу СТАРИТЕ Arduino прошивки (тестовите им драйвери
# в tests/native/esp32, събрани от ../esp32/run_esp32.sh). Python (библиотеката cryptography) е независим арбитър
# на wire формата. Пропуска се, ако старите драйвери не са събрани.
#
#   cd tests/native/espidf && ./run_compat.sh
set -e
cd "$(dirname "$0")"
. ./common.sh

INC="$INC_BASE -I compat"
build() { # build <име> <драйвер> <app> <допълнителни...>
  local name="$1" drv="$2" app="$3"; shift 3
  $CXX $INC -I "$A/$app/main" "$drv" $SIM $HAL $CORE "$@" $(ls "$A/$app/main/"*.cpp) -o "$OUT/$name.exe" 2>&1 | grep -E "error|Error" | head -20 || true
  [ -f "$OUT/$name.exe" ] || { echo "компилацията на $name се провали"; exit 1; }
}
rm -f "$OUT/compat_sensor.exe" "$OUT/compat_gateway.exe" "$OUT/compat_executor.exe"
build compat_sensor compat/compat_sensor.cpp sensor $RADIO
build compat_gateway compat/compat_gateway.cpp gateway $RADIO $NET
build compat_executor compat/compat_executor.cpp executor $RADIO
PYTHONIOENCODING=utf8 python test_compat.py
