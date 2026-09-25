#!/usr/bin/env bash
# Проверка на СИНТАКСИС и ТИПОВЕ (g++ -fsyntax-only) на радио-ниво файловете, които не могат
# да се изпълняват нативно (зависят от LoRa/SPI/ArduinoJson): не докажва поведението им, но
# хваща грешки при компилация (липсващи декларации, грешни типове, забравени includes,
# остатъчни препратки към премахнати константи). Ползва минимални заместители на LoRa.h/SPI.h;
# gateway/lora_handlers.cpp ползва РЕАЛНАТА ArduinoJson от библиотеките на машината.
cd "$(dirname "$0")"
SRC=../../firmware_src
ARDUINOJSON="D:/A1_WinData/Documents/Arduino/libraries/ArduinoJson/src"
CXX="g++ -std=c++17 -fsyntax-only -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function"

fail=0
check() {   # check <папка> <файл> [допълнителни -I]
  local dir="$1"; local file="$2"; shift 2
  if $CXX -I fakelibs -I "$SRC/$dir" "$@" "$SRC/$dir/$file" 2> /tmp/syn_err.txt; then
    echo "[OK]   $dir/$file"
  else
    echo "[FAIL] $dir/$file"; head -20 /tmp/syn_err.txt; fail=1
  fi
}

for d in sensor executor repeater gateway; do
  check $d cad.cpp
  check $d channel_access.cpp
  check $d radio_timing.cpp
done
# (gateway/config_storage.cpp ползва ESP32 Preferences.h - не се проверява тук)
for d in sensor executor repeater; do
  check $d config_storage.cpp
done

check sensor radio_tx.cpp
check executor radio_io.cpp
check repeater radio_io.cpp
check gateway lora_handlers.cpp -I "$ARDUINOJSON"

if [ $fail -eq 0 ]; then echo "SYNTAX CHECK OK"; else echo "SYNTAX CHECK: ИМА ГРЕШКИ"; exit 1; fi
