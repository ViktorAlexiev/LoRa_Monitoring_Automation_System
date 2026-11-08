# Радио слой: SX1262 през RadioLib (компонент от регистъра на ESP-IDF; виж idf_component.yml).
idf_component_register(
    SRCS "agro_radio.cpp"
    INCLUDE_DIRS "include"
    REQUIRES agro_hal agro_core agro_board esp_driver_gpio esp_driver_spi log jgromes__radiolib)
