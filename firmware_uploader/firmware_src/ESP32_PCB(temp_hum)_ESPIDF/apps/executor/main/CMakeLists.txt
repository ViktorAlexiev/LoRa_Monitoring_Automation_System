idf_component_register(
    SRCS "app_main.cpp" "exec_config.cpp" "exec_radio.cpp" "exec_cmd.cpp"
    INCLUDE_DIRS "."
    REQUIRES agro_hal agro_core agro_radio agro_board freertos esp_timer log)
