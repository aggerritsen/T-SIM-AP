idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        driver
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_uart
        esp_event
        esp_http_server
        esp_netif
        esp_timer
        esp_wifi
        lwip
        nvs_flash
)
