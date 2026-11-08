idf_component_register(
    SRCS "app_main.cpp" "rep_config.cpp" "rep_radio.cpp"
    INCLUDE_DIRS "."
    REQUIRES agro_hal agro_core agro_radio agro_board freertos esp_timer log)
