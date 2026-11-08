idf_component_register(
    SRCS "wifi_link.cpp" "mqtt_link.cpp"
    INCLUDE_DIRS "include"
    REQUIRES agro_hal agro_core esp_wifi esp_netif esp_event mqtt nvs_flash log)
