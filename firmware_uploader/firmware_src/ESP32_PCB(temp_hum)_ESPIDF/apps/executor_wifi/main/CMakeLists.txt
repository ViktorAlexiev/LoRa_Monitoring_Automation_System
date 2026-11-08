idf_component_register(
    SRCS "app_main.cpp" "ew_config.cpp" "ew_logic.cpp"
    INCLUDE_DIRS "."
    REQUIRES agro_hal agro_core agro_net agro_board freertos log)
