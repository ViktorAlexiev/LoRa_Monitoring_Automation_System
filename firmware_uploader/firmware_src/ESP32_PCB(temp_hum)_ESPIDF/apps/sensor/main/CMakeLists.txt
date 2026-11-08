idf_component_register(
    SRCS "app_main.cpp" "sensor_config.cpp" "sensors_io.cpp" "sensor_tx.cpp"
    INCLUDE_DIRS "."
    REQUIRES agro_hal agro_core agro_radio agro_board esp_driver_i2c esp_driver_gpio log)
