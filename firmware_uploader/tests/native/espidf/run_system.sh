#!/usr/bin/env bash
# Системна симулация: РЕАЛНИТЕ приложения (sensor, repeater, gateway, executor, executor_wifi) като отделни процеси
# в общ виртуален час и общ радио ефир (Python координатор). Показва, че съобщенията минават между ВСИЧКИ модули.
#
#   cd tests/native/espidf && ./run_system.sh
set -e
cd "$(dirname "$0")"
. ./common.sh

INC="$INC_BASE -I system"
build() { # build <име> <app> <допълнителни...>
  local name="$1" app="$2"; shift 2
  rm -f "$OUT/$name.exe"
  $CXX $INC -I "$A/$app/main" "system/$name.cpp" $SIM $HAL $CORE "$@" $(ls "$A/$app/main/"*.cpp) -o "$OUT/$name.exe" > "$OUT/$name.log" 2>&1 || { grep -E "error" "$OUT/$name.log" | head -20; echo "компилацията на $name се провали"; exit 1; }
}
build sys_sensor sensor $RADIO
build sys_repeater repeater $RADIO
build sys_executor executor $RADIO
build sys_gateway gateway $RADIO $NET
build sys_executor_wifi executor_wifi $RADIO $NET
PYTHONIOENCODING=utf8 python test_system.py "$@"
