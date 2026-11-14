/*
 * Bewegungs-Aufnahmen auf die MicroSD-Karte als AVI-Datei.
 *  - ESP32-S3: MJPEG (fourcc MJPG) - laeuft in jedem Player
 *  - ESP32-P4: H.264  (fourcc H264) - VLC, Windows "Medienwiedergabe", ffmpeg
 *
 * Der Video-Task gibt Bilder ab; ein eigener Task schreibt sie auf die Karte,
 * damit der Stream nicht auf die SD-Karte warten muss.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <inttypes.h>
#include "recorder.h"
#include "sdcard.h"
#include "app_config.h"
#include "mqtt_ctrl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

static const char *TAG = "rec";

#define QUEUE_LEN       24
#define MIN_FREE_BYTES  (300ULL * 1024 * 1024)   /* so viel muss vor einem Clip frei sein */
#define MAX_INDEX       (60 * 60 * 30)           /* max. Bilder pro Clip (1 h bei 30 fps) */

typedef enum { MSG_START, MSG_FRAME, MSG_STOP } msg_type_t;

typedef struct {
    msg_type_t type;
    uint8_t *data;
    uint32_t len;
    bool key;
} rec_msg_t;

typedef struct {
    uint32_t off;
    uint32_t size;
    uint32_t flags;
} idx_t;

static QueueHandle_t s_q;
static uint32_t s_w, s_h;
static rtsp_codec_t s_codec;
static bool s_ready;

/* Zustand im Video-Task */
static volatile bool s_rec;
static volatile bool s_want_start;
static volatile rec_reason_t s_want_reason;
static volatile rec_reason_t s_cur_reason;
static volatile bool s_stop_req;
static volatile int64_t s_block_until;
static volatile bool s_file_open;  /* nach Fehlern (Karte voll/defekt) erst spaeter neu versuchen */
static uint32_t s_bytes_per_s;         /* Datenrate der letzten Aufnahme (Schaetzung fuer den Platzbedarf) */
static volatile bool s_idr_req;
static bool s_wait_key;
static int64_t s_end_us;

static char s_last[64];
static unsigned s_count;

/* ------------------------------------------------------------------------- */
/* AVI                                                                        */
/* ------------------------------------------------------------------------- */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v;
    p[1] = v >> 8;
}

#define AVI_HDR_LEN 224

static void avi_header(uint8_t *h, uint32_t frames, uint32_t us_per_frame, uint32_t movi_size,
                       uint32_t riff_size, uint32_t max_frame)
{
    const char *fcc = (s_codec == RTSP_CODEC_H264) ? "H264" : "MJPG";
    uint32_t rate_x1000 = us_per_frame ? (uint32_t)(1000000000ULL / us_per_frame) : 15000;
    memset(h, 0, AVI_HDR_LEN);
    memcpy(h + 0, "RIFF", 4);
    put32(h + 4, riff_size);
    memcpy(h + 8, "AVI ", 4);
    memcpy(h + 12, "LIST", 4);
    put32(h + 16, 192);
    memcpy(h + 20, "hdrl", 4);
    memcpy(h + 24, "avih", 4);
    put32(h + 28, 56);
    put32(h + 32, us_per_frame);
    put32(h + 36, max_frame * (rate_x1000 / 1000 + 1));
    put32(h + 44, 0x10);                 /* AVIF_HASINDEX */
    put32(h + 48, frames);
    put32(h + 56, 1);                    /* Streams */
    put32(h + 60, max_frame);
    put32(h + 64, s_w);
    put32(h + 68, s_h);
    memcpy(h + 88, "LIST", 4);
    put32(h + 92, 116);
    memcpy(h + 96, "strl", 4);
    memcpy(h + 100, "strh", 4);
    put32(h + 104, 56);
    memcpy(h + 108, "vids", 4);
    memcpy(h + 112, fcc, 4);
    put32(h + 128, 1000);                /* dwScale */
    put32(h + 132, rate_x1000);          /* dwRate -> fps = Rate/Scale */
    put32(h + 140, frames);              /* dwLength */
    put32(h + 144, max_frame);
    put32(h + 148, 0xFFFFFFFF);          /* dwQuality */
    put16(h + 160, s_w);
    put16(h + 162, s_h);
    memcpy(h + 164, "strf", 4);
    put32(h + 168, 40);
    put32(h + 172, 40);                  /* BITMAPINFOHEADER */
    put32(h + 176, s_w);
    put32(h + 180, s_h);
    put16(h + 184, 1);
    put16(h + 186, 24);
    memcpy(h + 188, fcc, 4);
    put32(h + 192, s_w * s_h * 3);
    memcpy(h + 212, "LIST", 4);
    put32(h + 216, movi_size);
    memcpy(h + 220, "movi", 4);
}

/* ------------------------------------------------------------------------- */
/* Dateiverwaltung                                                            */
/* ------------------------------------------------------------------------- */

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(const char * const *)a, *(const char * const *)b);
}

/* Loescht die aeltesten Aufnahmen, bis genug Platz frei ist. */
static bool ensure_space(rec_reason_t reason)
{
    uint64_t total, freeb;
    sdcard_space(&total, &freeb);
    /* Platz fuer die naechste Aufnahme (Schaetzung aus der letzten Datenrate, mit Reserve) */
    int secs = (reason == REC_CONTINUOUS) ? g_cfg.rec_segment_s : g_cfg.rec_seconds;
    uint64_t need = (uint64_t)s_bytes_per_s * secs * 3 / 2;
    if (need < MIN_FREE_BYTES) {
        need = MIN_FREE_BYTES;
    }
    if (total && need > total / 2) {
        need = total / 2;
    }
    if (freeb >= need) {
        return true;
    }
    /* Daueraufnahme ist ein Ringspeicher: aelteste Dateien werden immer geloescht */
    if (!g_cfg.rec_autodelete && reason != REC_CONTINUOUS) {
        ESP_LOGW(TAG, "SD-Karte voll - Aufnahme uebersprungen (automatisches Loeschen ist aus)");
        return false;
    }
    DIR *d = opendir(SD_REC_DIR);
    if (!d) {
        return false;
    }
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type == DT_DIR) {
            continue;
        }
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            names = realloc(names, cap * sizeof(char *));
        }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof(char *), cmp_names);
    for (int i = 0; i < n && freeb < need; i++) {
        char path[300];
        snprintf(path, sizeof(path), SD_REC_DIR "/%s", names[i]);
        if (strcmp(names[i], s_last) != 0 && unlink(path) == 0) {
            ESP_LOGI(TAG, "Speicher voll - geloescht: %s", names[i]);
        }
        sdcard_space(&total, &freeb);
    }
    for (int i = 0; i < n; i++) {
        free(names[i]);
    }
    free(names);
    return freeb >= need;
}

static const char *reason_suffix(rec_reason_t r)
{
    switch (r) {
    case REC_MOTION:     return "bewegung";
    case REC_MANUAL:     return "manuell";
    case REC_CONTINUOUS: return "dauer";
    default:             return "aufnahme";
    }
}

static void make_name(char *out, size_t len, rec_reason_t reason)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year + 1900 >= 2024) {
        char ts[24];
        strftime(ts, sizeof(ts), "%Y-%m-%d_%H-%M-%S", &tm);
        snprintf(out, len, "%s_%s.avi", ts, reason_suffix(reason));
    } else {
        /* Uhrzeit unbekannt (kein NTP) - fortlaufende Nummer */
        static unsigned boot_id;
        if (!boot_id) {
            boot_id = (unsigned)(esp_timer_get_time() & 0xFFFF) | 1;
        }
        snprintf(out, len, "ohne-Uhrzeit_%04X_%03u_%s.avi", boot_id, s_count + 1, reason_suffix(reason));
    }
}

/* ------------------------------------------------------------------------- */
/* Schreib-Task                                                               */
/* ------------------------------------------------------------------------- */

static void writer_task(void *arg)
{
    FILE *f = NULL;
    idx_t *idx = NULL;
    uint32_t frames = 0, movi_pos = 4, max_frame = 0;
    int64_t t_start = 0;
    char path[96] = "";
    uint8_t hdr[AVI_HDR_LEN];
    uint8_t chunk[8];
    static const uint8_t zero[1] = {0};

    for (;;) {
        rec_msg_t m;
        xQueueReceive(s_q, &m, portMAX_DELAY);

        if (m.type == MSG_START) {
            if (f) {
                continue;
            }
            rec_reason_t reason = (rec_reason_t)m.len;
            if (!ensure_space(reason)) {
                s_block_until = esp_timer_get_time() + 30LL * 1000000;
                s_rec = false;
                continue;
            }
            char name[64];
            make_name(name, sizeof(name), reason);
            snprintf(path, sizeof(path), SD_REC_DIR "/%s", name);
            /* Name schon vergeben (zwei Aufnahmen in derselben Sekunde)? -> _2, _3 ... anhaengen */
            struct stat st;
            char base[64];
            strlcpy(base, name, sizeof(base));
            char *dot = strrchr(base, '.');
            if (dot) {
                *dot = 0;
            }
            for (int k = 2; stat(path, &st) == 0 && k < 100; k++) {
                snprintf(name, sizeof(name), "%.50s_%d.avi", base, k);
                snprintf(path, sizeof(path), SD_REC_DIR "/%s", name);
            }
            f = fopen(path, "wb");
            if (!f) {
                ESP_LOGE(TAG, "Datei %s kann nicht angelegt werden", path);
                s_block_until = esp_timer_get_time() + 30LL * 1000000;
                s_rec = false;
                continue;
            }
            setvbuf(f, NULL, _IOFBF, 32 * 1024);
            s_file_open = true;
            if (!idx) {
                idx = heap_caps_malloc(MAX_INDEX * sizeof(idx_t), MALLOC_CAP_SPIRAM);
            }
            avi_header(hdr, 0, 66666, 4, 0, 0);
            fwrite(hdr, 1, AVI_HDR_LEN, f);
            frames = 0;
            movi_pos = 4;
            max_frame = 0;
            t_start = esp_timer_get_time();
            strlcpy(s_last, name, sizeof(s_last));
            ESP_LOGI(TAG, "Aufnahme gestartet: %s", name);
            mqtt_ctrl_publish("Aufnahme", name, false);
        } else if (m.type == MSG_FRAME) {
            if (f && idx && frames < MAX_INDEX) {
                memcpy(chunk, "00dc", 4);
                put32(chunk + 4, m.len);
                bool ok = fwrite(chunk, 1, 8, f) == 8 && fwrite(m.data, 1, m.len, f) == m.len;
                if (m.len & 1) {
                    fwrite(zero, 1, 1, f);
                }
                if (ok) {
                    idx[frames].off = movi_pos;
                    idx[frames].size = m.len;
                    idx[frames].flags = m.key ? 0x10 : 0;
                    frames++;
                    movi_pos += 8 + m.len + (m.len & 1);
                    if (m.len > max_frame) {
                        max_frame = m.len;
                    }
                } else {
                    ESP_LOGE(TAG, "Schreibfehler auf der SD-Karte - Aufnahme beendet");
                    s_rec = false;
                    xQueueSend(s_q, &(rec_msg_t){ .type = MSG_STOP }, 0);
                }
            }
            free(m.data);
        } else if (m.type == MSG_STOP) {
            if (!f) {
                continue;
            }
            /* Index schreiben */
            memcpy(chunk, "idx1", 4);
            put32(chunk + 4, frames * 16);
            fwrite(chunk, 1, 8, f);
            for (uint32_t i = 0; i < frames; i++) {
                uint8_t e[16];
                memcpy(e, "00dc", 4);
                put32(e + 4, idx[i].flags);
                put32(e + 8, idx[i].off);
                put32(e + 12, idx[i].size);
                fwrite(e, 1, 16, f);
            }
            int64_t dur = esp_timer_get_time() - t_start;
            uint32_t us_pf = frames ? (uint32_t)(dur / frames) : 66666;
            uint32_t movi_size = movi_pos;                       /* 'movi' + Chunks */
            uint32_t riff_size = (AVI_HDR_LEN - 8) + (movi_pos - 4) + 8 + frames * 16;
            avi_header(hdr, frames, us_pf, movi_size, riff_size, max_frame);
            fseek(f, 0, SEEK_SET);
            fwrite(hdr, 1, AVI_HDR_LEN, f);
            fclose(f);
            f = NULL;
            s_file_open = false;
            s_count++;
            if (dur > 2000000) {
                s_bytes_per_s = (uint32_t)((uint64_t)movi_pos * 1000000 / dur);
            }
            ESP_LOGI(TAG, "Aufnahme beendet: %s (%" PRIu32 " Bilder, %.1f s, %.1f fps)", s_last, frames,
                     dur / 1e6, frames ? frames * 1e6 / dur : 0);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Aufruf aus dem Video-Task                                                  */
/* ------------------------------------------------------------------------- */

void recorder_on_frame(const uint8_t *data, size_t len, bool keyframe)
{
    if (!s_ready) {
        return;
    }
    int64_t now = esp_timer_get_time();

    /* Daueraufnahme: nach jedem Segment sofort das naechste beginnen */
    if (g_cfg.rec_continuous && !s_rec && !s_want_start && now >= s_block_until) {
        s_want_reason = REC_CONTINUOUS;
        s_want_start = true;
    }
    if (s_want_start && !s_rec && now >= s_block_until) {
        s_want_start = false;
        s_stop_req = false;
        s_rec = true;
        s_cur_reason = s_want_reason;
        s_wait_key = (s_codec == RTSP_CODEC_H264);
        s_idr_req = s_wait_key;
        int secs = (s_cur_reason == REC_CONTINUOUS) ? g_cfg.rec_segment_s : g_cfg.rec_seconds;
        s_end_us = now + (int64_t)secs * 1000000;
        rec_msg_t m = { .type = MSG_START, .len = (uint32_t)s_cur_reason };
        xQueueSend(s_q, &m, portMAX_DELAY);
    }
    if (!s_rec) {
        return;
    }
    if (now >= s_end_us || s_stop_req) {
        s_stop_req = false;
        s_rec = false;
        s_cur_reason = REC_NONE;
        rec_msg_t m = { .type = MSG_STOP };
        xQueueSend(s_q, &m, portMAX_DELAY);
        return;
    }
    if (s_wait_key) {
        if (!keyframe) {
            return;
        }
        s_wait_key = false;
    }
    uint8_t *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!copy) {
        return;
    }
    memcpy(copy, data, len);
    rec_msg_t m = { .type = MSG_FRAME, .data = copy, .len = len, .key = keyframe };
    if (xQueueSend(s_q, &m, 0) != pdTRUE) {
        free(copy);                       /* SD-Karte zu langsam - Bild auslassen */
        ESP_LOGW(TAG, "SD-Karte zu langsam, Bild verworfen");
        if (s_codec == RTSP_CODEC_H264) {
            s_wait_key = true;            /* bei H.264 bis zum naechsten IDR warten */
            s_idr_req = true;
        }
    }
}

void recorder_motion(void)
{
    if (s_ready && g_cfg.rec_enable && !s_rec && !s_want_start) {
        s_want_reason = REC_MOTION;
        s_want_start = true;
    }
}

esp_err_t recorder_start_manual(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_rec && !s_want_start) {
        s_want_reason = REC_MANUAL;
        s_want_start = true;
    }
    return ESP_OK;
}

esp_err_t recorder_set_continuous(bool on)
{
    if (on && !s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (g_cfg.rec_continuous != on) {
        g_cfg.rec_continuous = on;
        config_save(&g_cfg);
        ESP_LOGI(TAG, "Daueraufnahme %s", on ? "EIN (Ringspeicher)" : "AUS");
    }
    if (!on && s_rec && s_cur_reason == REC_CONTINUOUS) {
        s_stop_req = true;                /* laufendes Segment sauber abschliessen */
    } else if (on && s_rec && s_cur_reason != REC_CONTINUOUS) {
        /* laufender Bewegungs-/Testclip laeuft zu Ende, danach beginnt die Daueraufnahme */
    }
    return ESP_OK;
}

bool recorder_continuous(void)
{
    return s_ready && g_cfg.rec_continuous;
}

rec_reason_t recorder_reason(void)
{
    return s_rec ? s_cur_reason : REC_NONE;
}

const char *recorder_reason_text(void)
{
    switch (recorder_reason()) {
    case REC_MOTION:     return "Bewegung";
    case REC_MANUAL:     return "manuell";
    case REC_CONTINUOUS: return "Daueraufnahme";
    default:             return "keine";
    }
}

void recorder_finish(int timeout_ms)
{
    if (!s_ready) {
        return;
    }
    s_block_until = INT64_MAX;                       /* keine neuen Aufnahmen mehr */
    if (s_rec) {
        s_stop_req = true;
    }
    for (int t = 0; t < timeout_ms && (s_rec || s_file_open); t += 50) {
        if (s_rec && s_stop_req) {
            /* Video-Task schickt MSG_STOP beim naechsten Bild; falls keins kommt, selbst senden */
            if (t >= 500) {
                s_rec = false;
                rec_msg_t m = { .type = MSG_STOP };
                xQueueSend(s_q, &m, 0);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "Rekorder angehalten%s", s_file_open ? " (Datei noch offen!)" : "");
}

bool recorder_ready(void)
{
    return s_ready;
}

bool recorder_take_idr_request(void)
{
    if (s_idr_req) {
        s_idr_req = false;
        return true;
    }
    return false;
}

bool recorder_active(void)
{
    return s_rec;
}

const char *recorder_last_file(void)
{
    return s_last;
}

unsigned recorder_count(void)
{
    return s_count;
}

esp_err_t recorder_init(uint32_t width, uint32_t height, rtsp_codec_t codec)
{
    if (!sdcard_ok()) {
        ESP_LOGW(TAG, "Keine SD-Karte - Aufnahmen deaktiviert");
        return ESP_ERR_INVALID_STATE;
    }
    s_w = width;
    s_h = height;
    s_codec = codec;
    s_q = xQueueCreate(QUEUE_LEN, sizeof(rec_msg_t));
    if (!s_q || xTaskCreatePinnedToCore(writer_task, "rec_writer", 6144, NULL, 4, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_ready = true;
    ESP_LOGI(TAG, "Rekorder bereit (%s, Clip %d s, Bewegung %s, Daueraufnahme %s)",
             codec == RTSP_CODEC_H264 ? "H.264/AVI" : "MJPEG/AVI", g_cfg.rec_seconds,
             g_cfg.rec_enable ? "an" : "aus", g_cfg.rec_continuous ? "an" : "aus");
    return ESP_OK;
}
