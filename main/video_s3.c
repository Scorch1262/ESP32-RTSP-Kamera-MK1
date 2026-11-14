/*
 * Videopfad ESP32-S3: OV5640 / OV2640 (DVP) liefert JPEG -> MJPEG ueber RTSP (RFC 2435)
 *
 * Der ESP32-S3 hat keinen H.264-Encoder. Die Kamera komprimiert das Bild selbst als JPEG,
 * das unveraendert per RTP verschickt wird.
 */
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "esp_camera.h"
#include "video.h"
#include "app_config.h"
#include "sdkconfig.h"

static const char *TAG = "video";

#ifdef CONFIG_APP_S3_CAM_VFLIP
#define CAM_VFLIP 1
#else
#define CAM_VFLIP 0
#endif
#ifdef CONFIG_APP_S3_CAM_HMIRROR
#define CAM_HMIRROR 1
#else
#define CAM_HMIRROR 0
#endif

static int s_fps_target;

typedef struct {
    int key;            /* Wert in der Konfiguration (Bildbreite, SXGA = 1281) */
    framesize_t fs;
    uint16_t w, h;
} fs_entry_t;

static const fs_entry_t s_sizes[] = {
    { 640,  FRAMESIZE_VGA,  640,  480 },
    { 800,  FRAMESIZE_SVGA, 800,  600 },
    { 1024, FRAMESIZE_XGA,  1024, 768 },
    { 1280, FRAMESIZE_HD,   1280, 720 },
    { 1281, FRAMESIZE_SXGA, 1280, 1024 },
    { 1600, FRAMESIZE_UXGA, 1600, 1200 },
    { 1920, FRAMESIZE_FHD,  1920, 1080 },
};

static const fs_entry_t *find_size(int key)
{
    for (size_t i = 0; i < sizeof(s_sizes) / sizeof(s_sizes[0]); i++) {
        if (s_sizes[i].key == key) {
            return &s_sizes[i];
        }
    }
    return &s_sizes[3];   /* HD */
}

static void stream_task(void *arg)
{
    int64_t stat_t0 = esp_timer_get_time();
    uint32_t stat_frames = 0;
    uint64_t stat_bytes = 0;

    for (;;) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "Kein Kamerabild");
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        uint32_t ts90k = (uint32_t)((esp_timer_get_time() * 9) / 100);
        if (rtsp_server_active_clients() > 0) {
            rtsp_server_push_jpeg(fb->buf, fb->len, ts90k);
        }
        stat_frames++;
        stat_bytes += fb->len;
        esp_camera_fb_return(fb);

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
    const fs_entry_t *fs = find_size(g_cfg.cam_width);
    int q = g_cfg.cam_quality;
    if (q < 4 || q > 40) {
        q = CONFIG_APP_S3_JPEG_QUALITY;
    }

    camera_config_t cfg = {
        .pin_pwdn = CONFIG_APP_S3_CAM_PWDN,      /* GPIO8 schaltet die Kameraversorgung (Low = ein) */
        .pin_reset = -1,
        .pin_xclk = CONFIG_APP_S3_CAM_XCLK,
        .pin_sccb_sda = CONFIG_APP_S3_CAM_SDA,
        .pin_sccb_scl = CONFIG_APP_S3_CAM_SCL,
        .pin_d7 = CONFIG_APP_S3_CAM_D7,
        .pin_d6 = CONFIG_APP_S3_CAM_D6,
        .pin_d5 = CONFIG_APP_S3_CAM_D5,
        .pin_d4 = CONFIG_APP_S3_CAM_D4,
        .pin_d3 = CONFIG_APP_S3_CAM_D3,
        .pin_d2 = CONFIG_APP_S3_CAM_D2,
        .pin_d1 = CONFIG_APP_S3_CAM_D1,
        .pin_d0 = CONFIG_APP_S3_CAM_D0,
        .pin_vsync = CONFIG_APP_S3_CAM_VSYNC,
        .pin_href = CONFIG_APP_S3_CAM_HREF,
        .pin_pclk = CONFIG_APP_S3_CAM_PCLK,
        .xclk_freq_hz = CONFIG_APP_S3_CAM_XCLK_HZ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = fs->fs,
        .jpeg_quality = q,
        .fb_count = 3,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init: %s - Kamera richtig eingesteckt?", esp_err_to_name(err));
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    const char *name = "Kamera";
    if (s) {
        camera_sensor_info_t *si = esp_camera_sensor_get_info(&s->id);
        if (si) {
            name = si->name;
        }
        if (s->id.PID == OV5640_PID) {
            s->set_vflip(s, CAM_VFLIP);
            s->set_hmirror(s, CAM_HMIRROR);
        }
    }

    /* Ein Testbild holen, um die tatsaechliche Groesse zu kennen */
    uint32_t w = fs->w, h = fs->h;
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
        w = fb->width;
        h = fb->height;
        esp_camera_fb_return(fb);
    }

    static char desc[64];
    snprintf(desc, sizeof(desc), "%s DVP, MJPEG (Qualitaet %d)", name, q);
    s_fps_target = (w * h > 1280 * 720) ? 8 : 15;

    info->width = w;
    info->height = h;
    info->fps = s_fps_target;
    info->codec = RTSP_CODEC_MJPEG;
    info->desc = desc;
    return ESP_OK;
}

void video_start(void)
{
    xTaskCreatePinnedToCore(stream_task, "stream", 6144, NULL, 6, NULL, 1);
}
