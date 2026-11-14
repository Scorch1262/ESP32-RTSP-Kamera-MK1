/*
 * Videopfad ESP32-S3: OV5640 / OV2640 (DVP) liefert JPEG -> MJPEG ueber RTSP (RFC 2435)
 *
 * Der ESP32-S3 hat keinen H.264-Encoder. Die Kamera komprimiert das Bild selbst als JPEG,
 * das unveraendert per RTP verschickt wird.
 */
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "esp_camera.h"
#include "video.h"
#include "app_config.h"
#include "recorder.h"
#include "motion.h"
#include "jpeg_decoder.h"
#include "img_converters.h"
#include "freertos/semphr.h"
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

/* Drehung: 180 Grad per Sensor (kostenlos), 90/270 Grad per Software:
   Kamera liefert RGB565, das Bild wird gedreht und neu als JPEG kodiert. */
#define SOFT_ROT_MAX_PIXELS (640 * 480)
static int s_rot;
static bool s_soft_rot;
static uint16_t *s_rotbuf;
static int s_soft_q;

/* Dreht ein RGB565-Bild um 90 (cw = true) bzw. 270 Grad im Uhrzeigersinn, in 16x16-Bloecken */
static void rotate_rgb565(const uint16_t *src, uint16_t *dst, int w, int h, bool cw)
{
    for (int by = 0; by < h; by += 16) {
        for (int bx = 0; bx < w; bx += 16) {
            int ye = by + 16 < h ? by + 16 : h;
            int xe = bx + 16 < w ? bx + 16 : w;
            for (int y = by; y < ye; y++) {
                const uint16_t *row = src + y * w;
                for (int x = bx; x < xe; x++) {
                    int dx = cw ? (h - 1 - y) : y;
                    int dy = cw ? x : (w - 1 - x);
                    dst[dy * h + dx] = row[x];
                }
            }
        }
    }
}

/* Bewegungserkennung: Kopie eines JPEG-Bildes, im eigenen Task auf 1/8 dekodiert */
#define MOTION_INTERVAL_US  (300 * 1000)
#define MOTION_JPEG_MAX     (512 * 1024)
static uint8_t *s_mjpeg;
static size_t s_mjpeg_len;
static volatile bool s_motion_busy;
static SemaphoreHandle_t s_motion_sem;
static unsigned s_decode_errors, s_decode_ok;
static int64_t s_last_err_log;

static void motion_task(void *arg)
{
    uint8_t *rgb = NULL, *gray = NULL;
    size_t rgb_cap = 0;
    for (;;) {
        xSemaphoreTake(s_motion_sem, portMAX_DELAY);
        esp_jpeg_image_cfg_t jc = {
            .indata = s_mjpeg,
            .indata_size = s_mjpeg_len,
            .out_format = JPEG_IMAGE_FORMAT_RGB888,
            .out_scale = JPEG_IMAGE_SCALE_1_8,
        };
        esp_jpeg_image_output_t info;
        if (esp_jpeg_get_image_info(&jc, &info) == ESP_OK) {
            size_t need = (size_t)info.width * info.height * 3;
            if (need > rgb_cap) {
                free(rgb);
                free(gray);
                rgb = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
                gray = heap_caps_malloc(need / 3, MALLOC_CAP_SPIRAM);
                rgb_cap = (rgb && gray) ? need : 0;
            }
            if (rgb_cap) {
                jc.outbuf = rgb;
                jc.outbuf_size = rgb_cap;
                jc.indata = s_mjpeg;
                jc.indata_size = s_mjpeg_len;
                esp_err_t derr = esp_jpeg_decode(&jc, &info);
                if (derr != ESP_OK) {
                    s_decode_errors++;
                    int64_t now = esp_timer_get_time();
                    if (now - s_last_err_log > 60LL * 1000000) {
                        s_last_err_log = now;
                        bool has_dht = memmem(s_mjpeg, s_mjpeg_len, "\xFF\xC4", 2) != NULL;
                        bool has_dri = memmem(s_mjpeg, s_mjpeg_len, "\xFF\xDD", 2) != NULL;
                        ESP_LOGW(TAG, "Bewegungserkennung: JPEG nicht dekodierbar (%s, bisher %u Fehler, "
                                 "Huffman-Tabellen %s, Restart-Marker %s)", esp_err_to_name(derr),
                                 s_decode_errors, has_dht ? "ja" : "nein", has_dri ? "ja" : "nein");
                    }
                } else {
                    int n = info.width * info.height;
                    for (int i = 0; i < n; i++) {
                        const uint8_t *p = rgb + i * 3;
                        gray[i] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
                    }
                    if (motion_feed(gray, info.width, info.height, g_cfg.motion_sens)) {
                        recorder_motion();
                    }
                    s_decode_ok++;
                }
            }
        }
        s_motion_busy = false;
    }
}

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
        int64_t now_us = esp_timer_get_time();
        uint32_t ts90k = (uint32_t)((now_us * 9) / 100);

        const uint8_t *jpg = fb->buf;
        size_t jlen = fb->len;
        uint8_t *soft_jpg = NULL;
        if (s_soft_rot) {
            int w = fb->width, h = fb->height;
            rotate_rgb565((const uint16_t *)fb->buf, s_rotbuf, w, h, s_rot == 90);
            esp_camera_fb_return(fb);
            fb = NULL;
            if (!fmt2jpg((uint8_t *)s_rotbuf, (size_t)w * h * 2, h, w, PIXFORMAT_RGB565, s_soft_q,
                         &soft_jpg, &jlen)) {
                ESP_LOGW(TAG, "JPEG-Kodierung des gedrehten Bildes fehlgeschlagen");
                continue;
            }
            jpg = soft_jpg;
        }

        if (rtsp_server_active_clients() > 0) {
            rtsp_server_push_jpeg(jpg, jlen, ts90k);
        }
        recorder_on_frame(jpg, jlen, true);

        static int64_t last_motion;
        if (s_mjpeg && !s_motion_busy && jlen <= MOTION_JPEG_MAX && now_us - last_motion >= MOTION_INTERVAL_US) {
            last_motion = now_us;
            memcpy(s_mjpeg, jpg, jlen);
            s_mjpeg_len = jlen;
            s_motion_busy = true;
            xSemaphoreGive(s_motion_sem);
        }
        stat_frames++;
        stat_bytes += jlen;
        if (fb) {
            esp_camera_fb_return(fb);
        }
        free(soft_jpg);

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
    s_rot = ((g_cfg.video_rotation % 360) + 360) % 360;
    s_soft_rot = (s_rot == 90 || s_rot == 270);
    if (s_soft_rot && (int)fs->w * fs->h > SOFT_ROT_MAX_PIXELS) {
        ESP_LOGW(TAG, "Drehung um %d Grad: Software-Kodierung, Aufloesung auf 640x480 begrenzt", s_rot);
        fs = find_size(640);
    }
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
        .pixel_format = s_soft_rot ? PIXFORMAT_RGB565 : PIXFORMAT_JPEG,
        .frame_size = fs->fs,
        .jpeg_quality = q,
        .fb_count = s_soft_rot ? 2 : 3,
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
        /* 180 Grad = vertikal kippen + horizontal spiegeln (im Sensor, ohne Rechenaufwand) */
        int flip = (s_rot == 180) ? 1 : 0;
        if (s->id.PID == OV5640_PID || flip) {
            s->set_vflip(s, CAM_VFLIP ^ flip);
            s->set_hmirror(s, CAM_HMIRROR ^ flip);
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

    if (s_soft_rot) {
        s_rotbuf = heap_caps_malloc((size_t)w * h * 2, MALLOC_CAP_SPIRAM);
        if (!s_rotbuf) {
            ESP_LOGE(TAG, "Kein Speicher fuer die Bilddrehung");
            return ESP_ERR_NO_MEM;
        }
        /* Sensor-Skala 4..40 (klein = gut) auf Encoder-Skala 0..100 (gross = gut) */
        s_soft_q = 100 - q * 2;
        s_soft_q = s_soft_q < 30 ? 30 : (s_soft_q > 95 ? 95 : s_soft_q);
        uint32_t t = w;
        w = h;
        h = t;
    }

    static char desc[80];
    if (s_rot) {
        snprintf(desc, sizeof(desc), "%s DVP, MJPEG, %d Grad gedreht%s", name, s_rot,
                 s_soft_rot ? " (Software)" : "");
    } else {
        snprintf(desc, sizeof(desc), "%s DVP, MJPEG (Qualitaet %d)", name, q);
    }
    s_fps_target = s_soft_rot ? 8 : ((w * h > 1280 * 720) ? 8 : 15);

    info->width = w;
    info->height = h;
    info->fps = s_fps_target;
    info->codec = RTSP_CODEC_MJPEG;
    info->desc = desc;
    return ESP_OK;
}

void video_start(void)
{
    /* Fehlermeldungen des JPEG-Dekoders selbst unterdruecken - eigene, gedrosselte Meldung */
    esp_log_level_set("JPEG", ESP_LOG_NONE);
    s_mjpeg = heap_caps_malloc(MOTION_JPEG_MAX, MALLOC_CAP_SPIRAM);
    s_motion_sem = xSemaphoreCreateBinary();
    if (s_mjpeg && s_motion_sem) {
        xTaskCreatePinnedToCore(motion_task, "motion", 6144, NULL, 3, NULL, 0);
    }
    xTaskCreatePinnedToCore(stream_task, "stream", 6144, NULL, 6, NULL, 1);
}
