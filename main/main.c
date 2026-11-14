/*
 * P4-ETH-RTSP-Cam-MK1
 *
 * Waveshare ESP32-P4-ETH + OV5640 (MIPI-CSI) -> H.264 (Hardware) -> RTSP ueber Ethernet
 * Waveshare ESP32-S3-ETH + OV5640/OV2640 (DVP) -> MJPEG         -> RTSP ueber Ethernet (W5500)
 *
 * Stream-URL: rtsp://<IP-Adresse>:554/stream
 */
#include <stdio.h>
#include <string.h>
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
#include "status_led.h"
#include "wifi_ctrl.h"
#include "sdcard.h"
#include "recorder.h"
#include "motion.h"
#include "esp_netif_sntp.h"
#include <time.h>
#include <stdlib.h>
#include "sdkconfig.h"

static const char *TAG = "main";

static bool s_cam_ok;

static const char *state_text(eth_state_t st)
{
    switch (st) {
    case ETH_STATE_ERROR:   return "FEHLER - Ethernet-Chip nicht gefunden";
    case ETH_STATE_NO_LINK: return "kein Link - Netzwerkkabel pruefen";
    case ETH_STATE_LINK_UP: return "Link OK, warte auf IP-Adresse (DHCP)";
    case ETH_STATE_GOT_IP:  return "verbunden";
    }
    return "?";
}

/*
 * Status-LED und regelmaessige Statusausgabe, damit die IP-Adresse auch spaeter
 * noch im seriellen Monitor zu sehen ist.
 *   rot     = Ethernet-Fehler          gelb = Link, warte auf IP
 *   orange  = kein Netzwerkkabel        gruen = verbunden
 *   lila    = verbunden, aber Kamera-Fehler
 */
static void status_task(void *arg)
{
    int tick = 0;
    eth_state_t last = (eth_state_t)-1;
    bool motion_was = false;
    for (;;) {
        eth_state_t st = eth_get_state();
        const char *wip = wifi_ctrl_ip();
        if (st != ETH_STATE_GOT_IP && wip) {
            st = ETH_STATE_GOT_IP;            /* ueber WLAN erreichbar */
        }
        bool mot = motion_active();
        if (mot != motion_was) {
            motion_was = mot;
            mqtt_ctrl_publish("Bewegung", mot ? "1" : "0", true);
            if (mot) {
                ESP_LOGI(TAG, "Bewegung erkannt");
            }
        }
        bool blink = (tick & 1);
        switch (st) {
        case ETH_STATE_ERROR:   status_led_set(blink ? 40 : 0, 0, 0); break;
        case ETH_STATE_NO_LINK: status_led_set(30, 8, 0); break;
        case ETH_STATE_LINK_UP: status_led_set(blink ? 25 : 0, blink ? 20 : 0, 0); break;
        case ETH_STATE_GOT_IP:
            if (s_cam_ok) {
                status_led_set(0, 12, 0);
            } else {
                status_led_set(20, 0, 20);
            }
            break;
        }
        if (st != last || tick % (CONFIG_APP_STATUS_LOG_INTERVAL_S * 2) == 0) {
            const char *ip = eth_get_ip();
            if (!ip) {
                ip = wip;
            }
            if (ip) {
                if (s_cam_ok) {
                    ESP_LOGI(TAG, "Status: %s | IP %s | Web http://%s/ | Stream rtsp://%s:%d/%s",
                             state_text(st), ip, ip, ip, CONFIG_APP_RTSP_PORT, CONFIG_APP_RTSP_PATH);
                } else {
                    ESP_LOGW(TAG, "Status: %s | IP %s | Web http://%s/ | Kamera-Fehler, kein Stream",
                             state_text(st), ip, ip);
                }
            } else {
                ESP_LOGW(TAG, "Status: %s", state_text(st));
            }
            if (wifi_ctrl_available() && g_cfg.wifi_mode) {
                ESP_LOGI(TAG, "WLAN: %s", wifi_ctrl_state());
            }
            last = st;
        }
        tick++;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "P4-ETH-RTSP-Cam-MK1 v%s", app->version);

    status_led_init(CONFIG_APP_STATUS_LED_GPIO);
    status_led_set(0, 0, 30);   /* blau = Start */

    config_load(&g_cfg);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_err_t err = eth_start(&g_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet-Start fehlgeschlagen (%s) - Board laeuft ohne Netzwerk weiter", esp_err_to_name(err));
    }

    if (wifi_ctrl_start(&g_cfg) != ESP_OK && g_cfg.wifi_mode) {
        ESP_LOGW(TAG, "WLAN: %s", wifi_ctrl_state());
    }

    /* Uhrzeit per NTP (fuer Dateinamen der Aufnahmen), Zeitzone Deutschland */
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    if (g_cfg.ntp_server[0]) {
        esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG(g_cfg.ntp_server);
        esp_netif_sntp_init(&sc);
    }

    sdcard_mount();

    if (ir_light_init(g_cfg.ir_gpio, g_cfg.ir_active_low) != ESP_OK) {
        ESP_LOGE(TAG, "IR-Beleuchtung an GPIO %d konnte nicht eingerichtet werden", g_cfg.ir_gpio);
    }

    video_info_t vi = {0};
    err = video_init(&vi);
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
        if (rtsp_server_start(&rcfg) == ESP_OK) {
            recorder_init(vi.width, vi.height, vi.codec);
            video_start();
            s_cam_ok = true;
            ESP_LOGI(TAG, "Video: %s, %"PRIu32"x%"PRIu32, vi.desc, vi.width, vi.height);
        }
    }

    web_ui_start();
    xTaskCreate(status_task, "status", 3072, NULL, 3, NULL);

    /* Auf die erste IP-Adresse warten - Ethernet oder WLAN */
    char ip[16];
    for (;;) {
        if (eth_wait_for_ip(pdMS_TO_TICKS(1000), ip, sizeof(ip))) {
            break;
        }
        if (wifi_ctrl_ip()) {
            strlcpy(ip, wifi_ctrl_ip(), sizeof(ip));
            break;
        }
    }
    web_ui_set_ip(ip);
    mqtt_ctrl_start(&g_cfg, ip);

    ESP_LOGI(TAG, "==============================================");
    ESP_LOGI(TAG, " Webseite: http://%s/", ip);
    if (s_cam_ok) {
        ESP_LOGI(TAG, " Stream:   rtsp://%s:%d/%s", ip, CONFIG_APP_RTSP_PORT, CONFIG_APP_RTSP_PATH);
    }
    if (g_cfg.mqtt_uri[0]) {
        ESP_LOGI(TAG, " IR-Licht: %s = 0/1", g_cfg.mqtt_topic);
    }
    ESP_LOGI(TAG, "==============================================");
}
