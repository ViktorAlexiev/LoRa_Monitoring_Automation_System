idf_component_register(
    SRCS "app_main.cpp" "config_app.cpp"
    INCLUDE_DIRS "."
    REQUIRES agro_hal agro_core agro_cfg agro_board freertos log)
