set(AGRO_CORE_REQUIRES agro_hal agro_board freertos nvs_flash mbedtls esp_system esp_common log)

# cJSON: до ESP-IDF 5.x е вграден компонент "json"; от 6.0 е отделен компонент от регистъра
# (espressif/cjson, вижте idf_component.yml) и вече не е в REQUIRES.
if(IDF_VERSION_MAJOR LESS 6)
  list(APPEND AGRO_CORE_REQUIRES json)
endif()

idf_component_register(
    SRCS "crypto_common.cpp" "nvs_store.cpp" "ceiling_counter.cpp" "radio_timing.cpp"
         "channel_access.cpp" "json_util.cpp" "consumers.cpp"
    INCLUDE_DIRS "include"
    REQUIRES ${AGRO_CORE_REQUIRES})
