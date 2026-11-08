idf_component_register(
    SRCS "app_main.cpp" "gw_config.cpp" "gw_radio.cpp" "gw_logic.cpp"
    INCLUDE_DIRS "."
    REQUIRES agro_hal agro_core agro_radio agro_net agro_board freertos log)
