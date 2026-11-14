set(srcs "main.c" "eth_init.c" "rtsp_server.c" "app_config.c" "ir_light.c" "mqtt_ctrl.c" "web_ui.c" "status_led.c"
         "sdcard.c" "motion.c" "recorder.c" "wifi_ctrl.c")
set(reqs esp_eth esp_netif esp_event esp_timer mbedtls lwip esp_app_format nvs_flash mqtt json
         esp_http_server esp_driver_gpio esp_driver_rmt
         fatfs sdmmc esp_driver_sdmmc vfs)

if(IDF_TARGET STREQUAL "esp32p4")
    list(APPEND srcs "video_p4.c" "camera.c")
    list(APPEND reqs esp_video esp_h264 esp_driver_ppa esp_mm)
elseif(IDF_TARGET STREQUAL "esp32s3")
    list(APPEND srcs "video_s3.c")
    list(APPEND reqs esp32-camera esp_jpeg esp_driver_spi esp_wifi)
else()
    message(FATAL_ERROR "Nur ESP32-P4 und ESP32-S3 werden unterstuetzt")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    PRIV_REQUIRES ${reqs}
)
