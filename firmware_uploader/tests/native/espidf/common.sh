# Общи настройки на хост билда на ESP-IDF прошивките (включва се от run_espidf.sh, run_compat.sh, run_system.sh).
#
#   FW   - път до прошивката (по подразбиране firmware_src/ESP32_PCB(temp_hum)_ESPIDF)
#   OUT  - папка за .exe файловете (мутационната проверка ползва временна)
#   LIBDIR - папка за събраните външни библиотеки (RadioLib, cJSON): споделена, не се събира наново за всяка мутация
#
# Какво се компилира: РЕАЛНИТЕ components/ и apps/<приложение>/main, РЕАЛНИЯТ RadioLib (с неговия EspHal за ESP-IDF) и
# cJSON, срещу заместителите на ESP-IDF в fake_idf/ (симулатор на FreeRTOS във виртуално време, модел на SX1262 върху SPI
# командите, NVS, GPIO, I2C, Wi-Fi, MQTT брокер, mbedtls).
FW=${FW:-"../../../firmware_src/ESP32_PCB(temp_hum)_ESPIDF"}
OUT=${OUT:-build}
LIBDIR=${LIBDIR:-build}
mkdir -p "$OUT" "$LIBDIR"

RLSRC=third_party/RadioLib/src
# Само SX126x остава в RadioLib (същото като radiolib_options.cmake в билда за чипа)
RLDEFS="-DESP_PLATFORM -DAGRO_HOST_TEST -DRADIOLIB_EXCLUDE_CC1101=1 -DRADIOLIB_EXCLUDE_NRF24=1 -DRADIOLIB_EXCLUDE_RF69=1 -DRADIOLIB_EXCLUDE_SX1231=1 -DRADIOLIB_EXCLUDE_SI443X=1 -DRADIOLIB_EXCLUDE_RFM2X=1 -DRADIOLIB_EXCLUDE_SX127X=1 -DRADIOLIB_EXCLUDE_SX128X=1 -DRADIOLIB_EXCLUDE_AFSK=1 -DRADIOLIB_EXCLUDE_AX25=1 -DRADIOLIB_EXCLUDE_HELLSCHREIBER=1 -DRADIOLIB_EXCLUDE_MORSE=1 -DRADIOLIB_EXCLUDE_RTTY=1 -DRADIOLIB_EXCLUDE_SSTV=1 -DRADIOLIB_EXCLUDE_BELL=1 -DRADIOLIB_EXCLUDE_APRS=1 -DRADIOLIB_EXCLUDE_LORAWAN=1 -DRADIOLIB_EXCLUDE_LR11X0=1 -DRADIOLIB_EXCLUDE_FSK4=1 -DRADIOLIB_EXCLUDE_PAGER=1"

CXX="g++ -std=gnu++20 -O1 -g -static -Wall -Wextra -Wno-unused-parameter -Wno-unknown-pragmas -Wno-missing-field-initializers -Wno-unused-function -Wno-psabi $RLDEFS"
SIM="fake_idf/sim_rtos.cpp fake_idf/sim_periph.cpp fake_idf/sim_net.cpp"
C="$FW/components"
A="$FW/apps"
INC_BASE="-I fake_idf -I third_party/cjson -I $RLSRC -I .. -I $C/agro_hal/include -I $C/agro_core/include -I $C/agro_radio/include -I $C/agro_net/include -I $C/agro_cfg/include -I $C/agro_board/include"
HAL="$C/agro_hal/hal.cpp $C/agro_hal/power.cpp"
CORE="$C/agro_core/crypto_common.cpp $C/agro_core/nvs_store.cpp $C/agro_core/ceiling_counter.cpp $C/agro_core/radio_timing.cpp $C/agro_core/channel_access.cpp $C/agro_core/json_util.cpp $LIBDIR/cJSON.o $C/agro_core/consumers.cpp"
RADIO="$C/agro_radio/agro_radio.cpp $LIBDIR/libradiolib.a"
NET="$C/agro_net/wifi_link.cpp $C/agro_net/mqtt_link.cpp"
CFG="$C/agro_cfg/cfg_parse.cpp"

# cJSON (като C) и RadioLib (статична библиотека) се събират веднъж; нов билд при промяна на изходния код на цялата папка
build_third_party() {
  if [ ! -f "$LIBDIR/cJSON.o" ] || [ third_party/cjson/cJSON.c -nt "$LIBDIR/cJSON.o" ]; then
    gcc -O1 -w -c third_party/cjson/cJSON.c -o "$LIBDIR/cJSON.o"
  fi
  if [ ! -f "$LIBDIR/libradiolib.a" ] || [ "$(find $RLSRC -newer "$LIBDIR/libradiolib.a" -name '*.cpp' | head -1)" != "" ]; then
    echo "(събирам RadioLib - еднократно)"
    rm -rf "$LIBDIR/rl" "$LIBDIR/libradiolib.a"
    mkdir -p "$LIBDIR/rl"
    local n=0 f o
    for f in $(find $RLSRC -name '*.cpp' ! -path '*/hal/Arduino/*' ! -path '*/hal/RPi/*' ! -path '*/hal/RPiPico/*' ! -path '*/hal/Stm32duino/*'); do
      o="$LIBDIR/rl/$(echo "$f" | tr '/' '_').o"
      g++ -std=gnu++20 -O1 -w $RLDEFS -I fake_idf -I $RLSRC -c "$f" -o "$o" &
      n=$((n + 1))
      if [ $((n % 8)) -eq 0 ]; then wait; fi
    done
    wait
    ar rcs "$LIBDIR/libradiolib.a" "$LIBDIR"/rl/*.o
  fi
}
build_third_party

app_srcs() {   # всички .cpp от main/ на приложението
  ls "$A/$1/main/"*.cpp 2>/dev/null || true
}
