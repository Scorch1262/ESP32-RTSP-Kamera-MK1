/*
 * Videopfad ESP32-P4: OV5640 (MIPI-CSI) -> YUV420 -> H.264-Hardware-Encoder -> RTSP
 */
#include <inttypes.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_h264_alloc.h"
#include "camera.h"
#include "video.h"
#include "recorder.h"
#include "motion.h"
#include "app_config.h"
#include "sdkconfig.h"

static const char *TAG = "video";

static camera_info_t s_cam;

#define MOTION_W            160
#define MOTION_H            90
#define MOTION_INTERVAL_US  (250 * 1000)
static uint8_t s_gray[MOTION_W * MOTION_H];

/* Luminanz aus dem YUV420-Layout O_UYY_E_VYY verkleinert auslesen:
   je Zeile Gruppen aus 3 Byte [U oder V, Y0, Y1] fuer 2 Bildpunkte. */
static void sample_luma(const uint8_t *yuv, uint32_t w, uint32_t h)
{
    const uint32_t stride = w * 3 / 2;
    for (int gy = 0; gy < MOTION_H; gy++) {
        uint32_t y = (uint32_t)gy * h / MOTION_H;
        const uint8_t *row = yuv + y * stride;
        for (int gx = 0; gx < MOTION_W; gx++) {
            uint32_t x = (uint32_t)gx * w / MOTION_W;
            s_gray[gy * MOTION_W + gx] = row[(x / 2) * 3 + 1 + (x & 1)];
        }
    }
}

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

        int64_t now_us = esp_timer_get_time();
        static int64_t last_motion;
        if (now_us - last_motion >= MOTION_INTERVAL_US) {
            last_motion = now_us;
            sample_luma(frame.data, s_cam.width, s_cam.height);
            if (motion_feed(s_gray, MOTION_W, MOTION_H, g_cfg.motion_sens)) {
                recorder_motion();
            }
        }

        bool idr_rtsp = rtsp_server_take_idr_request();
        bool idr_rec = recorder_take_idr_request();
        if ((idr_rtsp || idr_rec) && param) {
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

        bool is_idr = (out.frame_type == ESP_H264_FRAME_TYPE_IDR);
        rtsp_server_push_frame(out_buf, out.length, ts90k, is_idr);
        recorder_on_frame(out_buf, out.length, is_idr);

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

esp_err_t video_init(video_info_t *info)
{
    esp_err_t err = camera_init(&s_cam, g_cfg.video_rotation);
    if (err != ESP_OK) {
        return err;
    }
    info->width = s_cam.width;
    info->height = s_cam.height;
    info->fps = CONFIG_APP_CAM_FPS;
    info->codec = RTSP_CODEC_H264;
    static char desc[64];
    snprintf(desc, sizeof(desc), "OV5640 MIPI-CSI, H.264 (Hardware)%s",
             g_cfg.video_rotation == 90 ? ", 90 Grad gedreht" : g_cfg.video_rotation == 180 ? ", 180 Grad gedreht" :
             g_cfg.video_rotation == 270 ? ", 270 Grad gedreht" : "");
    info->desc = desc;
    return ESP_OK;
}

void video_start(void)
{
    /* Encoder-/Streaming-Task auf Kern 1 */
    xTaskCreatePinnedToCore(stream_task, "stream", 8192, NULL, 6, NULL, 1);
}
