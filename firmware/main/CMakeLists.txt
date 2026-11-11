file(GLOB app_srcs "${CMAKE_CURRENT_LIST_DIR}/*.c")

idf_component_register(
    SRCS ${app_srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES kestrel_core bt esp_wifi esp_netif esp_event esp_http_server
                  nvs_flash esp_adc esp_lcd driver usb esp_timer lwip
)

# TinyUSB reads its configuration from main/tusb_config.h.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
