/*
 * P4-ETH-RTSP-Cam-MK1
 *
 * Waveshare ESP32-P4-ETH + OV5640 (MIPI-CSI)  ->  H.264 (Hardware-Encoder)  ->  RTSP ueber Ethernet
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
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_h264_alloc.h"
#include "eth_init.h"
#include "camera.h"
#include "rtsp_server.h"
#include "app_config.h"
#include "ir_tx.h"
#include "mqtt_ctrl.h"
#include "web_ui.h"
#include "sdkconfig.h"

static const char *TAG = "main";

static camera_info_t s_cam;

static void stream_task(void *arg)
{
    esp_h264_enc_cfg_hw_t cfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
        .gop = CONFIG_APP_H264_GOP,
        .fps = CONFIG_APP_CAM_FPS,
        .res = {
            .width = s_cam.width,
            .height = s_cam.height,
        },
        .rc = {
            .bitrate = CONFIG_APP_H264_BITRATE,
            .qp_min = CONFIG_APP_H264_QP_MIN,
            .qp_max = CONFIG_APP_H264_QP_MAX,
        },
    };

    esp_h264_enc_handle_t enc = NULL;
    esp_h264_enc_param_hw_handle_t param = NULL;
    if (esp_h264_enc_hw_new(&cfg, &enc) != ESP_H264_ERR_OK ||
            esp_h264_enc_open(enc) != ESP_H264_ERR_OK) {
        ESP_LOGE(TAG, "H.264-Encoder konnte nicht gestartet werden");
        vTaskDelete(NULL);
        return;
    }
    esp_h264_enc_hw_get_param_hd(enc, &param);

    /* Ausgabepuffer: ein I-Frame bleibt bei sinnvollen QP-Werten deutlich unter der Rohbildgroesse */
    uint32_t out_cap = 0;
    uint8_t *out_buf = esp_h264_aligned_calloc(128, 1, s_cam.yuv_size / 2, &out_cap, ESP_H264_MEM_SPIRAM);
    if (!out_buf) {
        ESP_LOGE(TAG, "Kein Speicher fuer H.264-Ausgabepuffer");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "H.264: %"PRIu32"x%"PRIu32" @ %d fps, %d kbit/s, GOP %d",
             s_cam.width, s_cam.height, CONFIG_APP_CAM_FPS, CONFIG_APP_H264_BITRATE / 1000, CONFIG_APP_H264_GOP);

    int64_t stat_t0 = esp_timer_get_time();
    uint32_t stat_frames = 0;
    uint64_t stat_bytes = 0;

    for (;;) {
        camera_frame_t frame;
        if (camera_get_frame(&frame) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (rtsp_server_take_idr_request() && param) {
            esp_h264_enc_force_idr(&param->base);
        }

        uint32_t ts90k = (uint32_t)((frame.timestamp_us * 9) / 100);

        esp_h264_enc_in_frame_t in = {
            .raw_data = { .buffer = frame.data, .len = frame.len },
            .pts = ts90k,
        };
        esp_h264_enc_out_frame_t out = {
            .raw_data = { .buffer = out_buf, .len = out_cap },
        };
        esp_h264_err_t er = esp_h264_enc_process(enc, &in, &out);
        camera_put_frame(&frame);

        if (er != ESP_H264_ERR_OK) {
            ESP_LOGW(TAG, "Encoder-Fehler %d", er);
            continue;
        }

        rtsp_server_push_frame(out_buf, out.length, ts90k, out.frame_type == ESP_H264_FRAME_TYPE_IDR);

        stat_frames++;
        stat_bytes += out.length;
        int64_t now = esp_timer_get_time();
        if (now - stat_t0 >= 10 * 1000000LL) {
            float sec = (now - stat_t0) / 1e6f;
            ESP_LOGI(TAG, "%.1f fps, %.0f kbit/s, %d Client(s), freier PSRAM %u kB",
                     stat_frames / sec, stat_bytes * 8 / sec / 1000, rtsp_server_active_clients(),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            stat_t0 = now;
            stat_frames = 0;
            stat_bytes = 0;
        }
    }
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "P4-ETH-RTSP-Cam-MK1 v%s", app->version);

    config_load(&g_cfg);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(eth_start(&g_cfg));

    if (ir_tx_init(g_cfg.ir_gpio) != ESP_OK) {
        ESP_LOGE(TAG, "IR-Sender an GPIO %d konnte nicht gestartet werden", g_cfg.ir_gpio);
    }

    bool cam_ok = false;
    esp_err_t err = camera_init(&s_cam);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Kamera-Initialisierung fehlgeschlagen (%s) - laufe ohne Videostream weiter",
                 esp_err_to_name(err));
    } else {
        rtsp_server_config_t rcfg = {
            .port = CONFIG_APP_RTSP_PORT,
            .path = CONFIG_APP_RTSP_PATH,
            .max_clients = CONFIG_APP_RTSP_MAX_CLIENTS,
            .fps = CONFIG_APP_CAM_FPS,
            .width = s_cam.width,
            .height = s_cam.height,
        };
        ESP_ERROR_CHECK(rtsp_server_start(&rcfg));
        /* Encoder-/Streaming-Task auf Kern 1 */
        xTaskCreatePinnedToCore(stream_task, "stream", 8192, NULL, 6, NULL, 1);
        cam_ok = true;
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
        ESP_LOGI(TAG, " IR-Topic: %s/ir/send", g_cfg.mqtt_topic);
    }
    ESP_LOGI(TAG, "==============================================");
}
