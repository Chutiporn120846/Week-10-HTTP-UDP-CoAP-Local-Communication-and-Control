idf_component_register(
    SRCS "Lab10-1_HTTP_REST_Server_New.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server esp_wifi esp_event nvs_flash esp_adc driver esp_driver_gpio mdns espressif__cjson
)