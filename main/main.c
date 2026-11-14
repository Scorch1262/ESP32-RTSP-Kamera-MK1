/*
 * P4-ETH-RTSP-Cam-MK1
 *
 * Waveshare ESP32-P4-ETH + OV5640 (MIPI-CSI) -> H.264 (Hardware) -> RTSP ueber Ethernet
 * Waveshare ESP32-S3-ETH + OV5640/OV2640 (DVP) -> MJPEG         -> RTSP ueber Ethernet (W5500)
 *
 * Stream-URL: rtsp://<IP-Adresse>:554/stream
 */
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "eth_init.h"
#include "rtsp_server.h"
#include "video.h"
#include "app_config.h"
#include "ir_light.h"
#include "mqtt_ctrl.h"
#include "web_ui.h"
#include "sdkconfig.h"

static const char *TAG = "main";

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "P4-ETH-RTSP-Cam-MK1 v%s", app->version);

    config_load(&g_cfg);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(eth_start(&g_cfg));

    if (ir_light_init(g_cfg.ir_gpio, g_cfg.ir_active_low) != ESP_OK) {
        ESP_LOGE(TAG, "IR-Beleuchtung an GPIO %d konnte nicht eingerichtet werden", g_cfg.ir_gpio);
    }

    bool cam_ok = false;
    video_info_t vi = {0};
    esp_err_t err = video_init(&vi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Kamera-Initialisierung fehlgeschlagen (%s) - laufe ohne Videostream weiter",
                 esp_err_to_name(err));
    } else {
        rtsp_server_config_t rcfg = {
            .port = CONFIG_APP_RTSP_PORT,
            .path = CONFIG_APP_RTSP_PATH,
            .max_clients = CONFIG_APP_RTSP_MAX_CLIENTS,
            .fps = vi.fps,
            .width = vi.width,
            .height = vi.height,
            .codec = vi.codec,
            .info = vi.desc,
        };
        ESP_ERROR_CHECK(rtsp_server_start(&rcfg));
        video_start();
        cam_ok = true;
        ESP_LOGI(TAG, "Video: %s, %"PRIu32"x%"PRIu32, vi.desc, vi.width, vi.height);
    }

    web_ui_start();

    char ip[16];
    while (!eth_wait_for_ip(pdMS_TO_TICKS(10000), ip, sizeof(ip))) {
        ESP_LOGW(TAG, "Warte auf Ethernet-Link / IP-Adresse ...");
    }
    web_ui_set_ip(ip);
    mqtt_ctrl_start(&g_cfg, ip);

    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, " Webseite: http://%s/", ip);
    if (cam_ok) {
        ESP_LOGI(TAG, " Stream:   rtsp://%s:%d/%s", ip, CONFIG_APP_RTSP_PORT, CONFIG_APP_RTSP_PATH);
    }
    if (g_cfg.mqtt_uri[0]) {
        ESP_LOGI(TAG, " IR-Licht: %s = 0/1", g_cfg.mqtt_topic);
    }
    ESP_LOGI(TAG, "==============================================");
}
