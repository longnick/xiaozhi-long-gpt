idf_component_register(
    SRCS "main.c" "gc9a01.c" "font6x8.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi nvs_flash esp_netif esp_event esp_timer esp_http_client esp_lcd esp_pm esp-tls mbedtls
)
