#!/usr/bin/env bash
# Мутационна проверка на тестовете: на ВРЕМЕННО копие на firmware_src/ESP32_PCB(temp_hum) се въвежда
# по една умишлена грешка и run_esp32.sh трябва да се ПРОВАЛИ. Ако някоя мутация не се улови,
# значи тестовете не пазят съответното поведение. Реалните файлове не се пипат.
#
#   ./mutation_check.sh [папка за временното копие]
cd "$(dirname "$0")"
SRCD="../../../firmware_src/ESP32_PCB(temp_hum)"
MUT=${1:-"${TMPDIR:-/tmp}/esp32_mut"}
caught=0; missed=0

mutate() {   # mutate <описание> <sed-израз> <файл>...
  local desc="$1" expr="$2"; shift 2
  rm -rf "$MUT"; mkdir -p "$MUT"; cp -r "$SRCD"/. "$MUT"/
  local applied=0
  for f in "$@"; do
    sed -i "$expr" "$MUT/$f"
    cmp -s "$SRCD/$f" "$MUT/$f" || applied=1
  done
  if [ $applied -eq 0 ]; then echo "!!  мутацията не се приложи: $desc"; missed=$((missed+1)); return; fi
  local out code
  out=$(ESP="$MUT" bash run_esp32.sh 2>&1); code=$?
  if [ $code -ne 0 ]; then
    echo "УЛОВЕНА      [$desc]"
    echo "$out" | grep -E "\[FAIL\]|^FAIL:|РАЗЛИКА|error:" | head -2 | sed 's/^/                /'
    caught=$((caught+1))
  else
    echo "НЕ СЕ УЛАВЯ  [$desc]"; missed=$((missed+1))
  fi
}

mutate "gateway: publish без каста -> retain=true" 's/publish(topic, (const uint8_t\*)buf, n, false)/publish(topic, buf, n)/' gateway/wifi_mqtt.cpp
mutate "gateway: дедупликацията е изключена" 's/if (gwDedupSeen(buf, (uint8_t)len)) {/if (false) {/' gateway/lora_handlers.cpp
mutate "gateway: няма-данни е 255 вместо -32768" 's/(-32768.0f)/(255.0f)/' gateway/lora_handlers.cpp
mutate "gateway: ACK се приема без проверка на M_ID" 's/memcmp(p.M_ID, pendingCmd.M_ID, MODULE_ID_LEN) == 0/true/' gateway/lora_handlers.cpp
mutate "gateway: ACK се приема без проверка на C_ID" 's/memcmp(p.C_ID, pendingCmd.C_ID, 4) == 0/true/' gateway/lora_handlers.cpp
mutate "sensor: компресия x10 вместо x100" 's/v \* 100.0f/v * 10.0f/' sensor/radio_tx.cpp
mutate "sensor: грешка се праща като 0 вместо -32768" 's/return SENSOR_RAW_NODATA;/return 0;/' sensor/radio_tx.cpp
mutate "sensor: сън 14 минути вместо ~15" 's/#define WDT_CYCLES_BETWEEN_SEND  113/#define WDT_CYCLES_BETWEEN_SEND  100/' sensor/power_mgmt.h
mutate "ceiling: не се пише watermark в NVS" 's/p.putULong(key, committed);//' sensor/ceiling_counter.cpp executor/ceiling_counter.cpp repeater/ceiling_counter.cpp
mutate "ceiling: RTC състоянието не се ползва" 's/rtcMagic == CEIL_RTC_MAGIC/false/' sensor/ceiling_counter.cpp executor/ceiling_counter.cpp repeater/ceiling_counter.cpp
mutate "power: тик 80 s вместо 8 s" 's/8000000ULL/80000000ULL/' executor/power_mgmt.cpp repeater/power_mgmt.cpp
mutate "power: DIO0 на ниско ниво" 's/GPIO_INTR_HIGH_LEVEL/GPIO_INTR_LOW_LEVEL/' executor/power_mgmt.cpp repeater/power_mgmt.cpp
mutate "config sensor: честотата не се чете от NVS" 's/prefs.getULong("freq", 0)/0/' sensor/config_storage.cpp
mutate "config executor: пиновете се четат от грешен ключ" 's/"c%up"/"c%uq"/' executor/config_storage.cpp
mutate "config_esp32: няма проверка на sf" 's/if (sf < 7 || sf > 12)/if (false)/' config_esp32/config_esp32.ino
mutate "config_esp32: няма проверка на GPIO обхвата" 's/pinNum < 0 || pinNum > 39/false/' config_esp32/config_esp32.ino
mutate "config_esp32: невалиден ключ се приема тихо" 's/if (!hexToBytes(keyStr, probe, 16))/if (false)/' config_esp32/config_esp32.ino
mutate "executor_wifi: няма филтър по M_ID" 's/if (strcmp(m_id, MY_M_ID) != 0) return;/if (false) return;/' executor_wifi/mqtt_exec.cpp
mutate "executor_wifi: clean session изключена" 's/willMsgOff, true);/willMsgOff, false);/' executor_wifi/mqtt_exec.cpp
mutate "executor_wifi: абониране с QoS 0" 's/subscribe(TOPIC_WIFI_COMMANDS, 1)/subscribe(TOPIC_WIFI_COMMANDS, 0)/' executor_wifi/mqtt_exec.cpp
mutate "executor_wifi: ACK статус 1 вместо 0" 's/#define STATUS_ACK     0/#define STATUS_ACK     1/' executor_wifi/mqtt_exec.h
mutate "executor_wifi: restart state response при всяко свързване" 's/restartStateSent = true;//' executor_wifi/mqtt_exec.cpp
mutate "executor_wifi: състоянието не се чете от реалния пин" 's/digitalRead(consumerPins\[i\]) ? "ON" : "OFF"/"OFF"/' executor_wifi/mqtt_exec.cpp
mutate "executor_wifi: публикува се с retain=true" 's/(const uint8_t\*)buf, n, false)/(const uint8_t*)buf, n, true)/' executor_wifi/mqtt_exec.cpp

mutate "executor: командата не включва пина" 's/digitalWrite(consumerPins\[idx\], p.com == CMD_ON ? HIGH : LOW);//' executor/radio_io.cpp
mutate "executor: ACK статус 1 вместо 0" 's/#define STATUS_ACK  0/#define STATUS_ACK  1/' executor/radio_io.h
mutate "executor: приема команди за други M_ID" 's/if (memcmp(buf, MY_M_ID, MODULE_ID_LEN) != 0) {/if (false) {/' executor/radio_io.cpp
mutate "executor: консуматорите не се изключват при boot" 's/digitalWrite(consumerPins\[i\], LOW);/digitalWrite(consumerPins[i], HIGH);/' executor/executor.ino
mutate "executor: restart response се повтаря 4 пъти" 's/#define STATE_RESP_MAX_RETRIES    2/#define STATE_RESP_MAX_RETRIES    4/' executor/radio_io.cpp
mutate "executor/repeater: heartbeat на 10 тика вместо 38" 's/#define HB_INTERVAL_CYCLES  38/#define HB_INTERVAL_CYCLES  10/' executor/power_mgmt.h repeater/power_mgmt.h
mutate "executor: state response без реалното състояние на пина" 's/(digitalRead(consumerPins\[i\]) == HIGH) ? 1 : 0;/0;/' executor/radio_io.cpp
mutate "repeater: препраща на RX честотата вместо TX" 's/LoRa.setFrequency(LORA_FREQ_TX_HZ);/LoRa.setFrequency(LORA_FREQ_RX_HZ);/' repeater/radio_io.cpp
mutate "repeater: дедупликацията е изключена" 's/if (dedupSeen(buf, i)) {/if (false) {/' repeater/radio_io.cpp
mutate "repeater: heartbeat не е с repeater type-id" 's/CRYPTO_TYPE_REPEATER_HB,/CRYPTO_TYPE_EXEC_HB,/' repeater/radio_io.cpp

echo
echo "Уловени: $caught   Неуловени: $missed"
rm -rf "$MUT"
[ $missed -eq 0 ]
