idf_component_register(
    SRCS "main.c" "eth_init.c" "camera.c" "rtsp_server.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_video esp_h264 esp_eth esp_netif esp_event esp_timer esp_driver_ppa esp_mm mbedtls lwip esp_app_format
)
