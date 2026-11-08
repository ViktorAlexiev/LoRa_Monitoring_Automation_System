idf_component_register(
    SRCS "hal.cpp" "power.cpp" "board_power.cpp"
    INCLUDE_DIRS "include"
    REQUIRES freertos agro_board esp_adc esp_driver_gpio esp_timer nvs_flash esp_pm esp_hw_support esp_rom esp_system log)
