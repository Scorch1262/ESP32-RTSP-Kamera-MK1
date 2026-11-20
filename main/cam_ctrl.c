/*
 * Kamera-Einstellungen: gemeinsamer Teil (Reglerliste, Speichern im NVS, Autofokus).
 * Die eigentlichen Registerzugriffe stehen in cam_ctrl_p4.c bzw. cam_ctrl_s3.c.
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "cam_ctrl.h"
#include "ov5640_af_fw.h"

static const char *TAG = "camctl";
#define NVS_NS "camctl"

static const char *const s_effects[] = { "Normal", "Negativ", "Graustufen", "Rotstich", "Gruenstich", "Blaustich", "Sepia" };
static const char *const s_wb[] = { "Automatisch", "Sonnig", "Bewoelkt", "Buero (Leuchtstoff)", "Zuhause (Gluehlampe)" };
static const char *const s_af[] = { "Aus (Linse auf Unendlich)", "Dauerhaft scharfstellen", "Einmalig (per Knopf)" };

static const cam_ctrl_def_t s_defs[CC_COUNT] = {
    [CC_BRIGHTNESS] = { "brightness", "Helligkeit", "Bild", CAMCTL_RANGE, -2, 2 },
    [CC_CONTRAST]   = { "contrast", "Kontrast", "Bild", CAMCTL_RANGE, -2, 2 },
    [CC_SATURATION] = { "saturation", "Farbsaettigung", "Bild", CAMCTL_RANGE, -2, 2 },
    [CC_SHARPNESS]  = { "sharpness", "Schaerfe", "Bild", CAMCTL_RANGE, -2, 2 },
    [CC_DENOISE]    = { "denoise", "Rauschunterdrueckung", "Bild", CAMCTL_RANGE, 0, 8, NULL, "0 = aus" },
    [CC_EFFECT]     = { "effect", "Effekt", "Bild", CAMCTL_SELECT, 0, 6, s_effects },
    [CC_AWB]        = { "awb", "Automatischer Weissabgleich", "Weissabgleich", CAMCTL_BOOL, 0, 1 },
    [CC_WB_MODE]    = { "wb_mode", "Weissabgleich-Modus", "Weissabgleich", CAMCTL_SELECT, 0, 4, s_wb },
    [CC_AEC]        = { "aec", "Automatische Belichtung", "Belichtung", CAMCTL_BOOL, 0, 1 },
    [CC_AE_LEVEL]   = { "ae_level", "Belichtungskorrektur", "Belichtung", CAMCTL_RANGE, -2, 2, NULL,
                        "wirkt bei automatischer Belichtung" },
    [CC_AEC_VALUE]  = { "aec_value", "Belichtungszeit (manuell)", "Belichtung", CAMCTL_RANGE, 0, 1200, NULL,
                        "nur bei ausgeschalteter automatischer Belichtung" },
    [CC_AGC]        = { "agc", "Automatische Verstaerkung", "Belichtung", CAMCTL_BOOL, 0, 1 },
    [CC_AGC_GAIN]   = { "agc_gain", "Verstaerkung (manuell)", "Belichtung", CAMCTL_RANGE, 0, 30, NULL,
                        "nur bei ausgeschalteter automatischer Verstaerkung" },
    [CC_NIGHT]      = { "night", "Nachtmodus (laengere Belichtung bei Dunkelheit)", "Belichtung", CAMCTL_BOOL, 0, 1, NULL,
                        "senkt bei wenig Licht die Bildrate" },
    [CC_LENC]       = { "lenc", "Objektiv-Korrektur (Randabschattung)", "Korrekturen", CAMCTL_BOOL, 0, 1 },
    [CC_BPC]        = { "bpc", "Pixelfehler-Korrektur (dunkle Pixel)", "Korrekturen", CAMCTL_BOOL, 0, 1 },
    [CC_WPC]        = { "wpc", "Pixelfehler-Korrektur (helle Pixel)", "Korrekturen", CAMCTL_BOOL, 0, 1 },
    [CC_GAMMA]      = { "gamma", "Gamma-Korrektur", "Korrekturen", CAMCTL_BOOL, 0, 1 },
    [CC_COLORBAR]   = { "colorbar", "Testbild (Farbbalken)", "Korrekturen", CAMCTL_BOOL, 0, 1 },
    [CC_AF]         = { "af", "Autofokus", "Fokus", CAMCTL_SELECT, 0, 2, s_af,
                        "nur wirksam bei Kameramodulen mit Fokusmotor" },
};

static bool s_ok;
static int s_val[CC_COUNT];
static int s_def[CC_COUNT];
static bool s_has[CC_COUNT];
static SemaphoreHandle_t s_lock;

/* ------------------------------------------------------------------------- */
/* Autofokus (OV5640, eigene Firmware im Sensor)                              */
/* ------------------------------------------------------------------------- */

#define AF_CMD_MAIN   0x3022
#define AF_CMD_ACK    0x3023
#define AF_CMD_PARA0  0x3024
#define AF_FW_STATUS  0x3029
#define AF_TRIG_SINGLE  0x03
#define AF_CONTINUOUS   0x04
#define AF_RELEASE      0x08

#define AF_REQ_MODE    (1 << 0)
#define AF_REQ_TRIGGER (1 << 1)

typedef enum { AF_FW_NONE = 0, AF_FW_LOADING, AF_FW_READY, AF_FW_ERROR } af_fw_state_t;
static volatile af_fw_state_t s_af_fw;
static TaskHandle_t s_af_task;

static esp_err_t af_wait_reg(uint16_t reg, uint8_t want, bool equal, uint32_t timeout_ms)
{
    int64_t start = esp_timer_get_time();
    for (;;) {
        uint8_t v;
        if (camb_reg_read(reg, &v) != ESP_OK) {
            return ESP_FAIL;
        }
        if ((v == want) == equal) {
            return ESP_OK;
        }
        if ((esp_timer_get_time() - start) / 1000 > timeout_ms) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static esp_err_t af_load_firmware(void)
{
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = camb_reg_write(0x3000, 0x20);          /* MCU anhalten */
    for (size_t i = 0; err == ESP_OK && i < sizeof(s_ov5640_af_fw); i++) {
        err = camb_reg_write(0x8000 + i, s_ov5640_af_fw[i]);
    }
    if (err != ESP_OK) {
        return err;
    }
    for (uint16_t r = AF_CMD_MAIN; r <= 0x3028; r++) {
        camb_reg_write(r, 0x00);
    }
    camb_reg_write(AF_FW_STATUS, 0x7f);
    camb_reg_write(0x3000, 0x00);                          /* MCU starten */
    err = af_wait_reg(AF_FW_STATUS, 0x70, true, 3000);
    ESP_LOGI(TAG, "AF-Firmware %s (%lld ms)", err == ESP_OK ? "geladen" : "reagiert nicht",
             (esp_timer_get_time() - t0) / 1000);
    return err;
}

static esp_err_t af_command(uint8_t cmd)
{
    esp_err_t err = camb_reg_write(AF_CMD_ACK, 0x01);
    if (err == ESP_OK) {
        err = camb_reg_write(AF_CMD_MAIN, cmd);
    }
    if (err == ESP_OK) {
        err = af_wait_reg(AF_CMD_ACK, 0x00, true, 2000);
    }
    return err;
}

static void af_task(void *arg)
{
    for (;;) {
        uint32_t req = 0;
        xTaskNotifyWait(0, UINT32_MAX, &req, portMAX_DELAY);
        int mode = s_val[CC_AF];
        bool trigger = (req & AF_REQ_TRIGGER) != 0;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if ((mode != 0 || trigger) && s_af_fw != AF_FW_READY) {
            s_af_fw = AF_FW_LOADING;
            s_af_fw = af_load_firmware() == ESP_OK ? AF_FW_READY : AF_FW_ERROR;
        }
        if (s_af_fw == AF_FW_READY) {
            esp_err_t err = ESP_OK;
            if (trigger || mode == 2) {
                err = af_command(AF_TRIG_SINGLE);
            } else if (mode == 1) {
                err = af_command(AF_CONTINUOUS);
            } else {
                err = af_command(AF_RELEASE);
            }
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "AF-Befehl nicht bestaetigt (%s)", esp_err_to_name(err));
            }
        }
        xSemaphoreGive(s_lock);
    }
}

static void af_request(uint32_t what)
{
    if (!s_af_task) {
        xTaskCreate(af_task, "cam_af", 3072, NULL, 2, &s_af_task);
    }
    if (s_af_task) {
        xTaskNotify(s_af_task, what, eSetBits);
    }
}

esp_err_t cam_ctrl_af_trigger(void)
{
    if (!s_ok || !s_has[CC_AF]) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    af_request(AF_REQ_TRIGGER);
    return ESP_OK;
}

void cam_ctrl_af_status(char *out, size_t len)
{
    if (!s_ok || !s_has[CC_AF]) {
        strlcpy(out, "nicht verfuegbar", len);
        return;
    }
    switch (s_af_fw) {
    case AF_FW_NONE:
        strlcpy(out, "aus", len);
        return;
    case AF_FW_LOADING:
        strlcpy(out, "Firmware wird geladen ...", len);
        return;
    case AF_FW_ERROR:
        strlcpy(out, "Fehler: AF-Firmware reagiert nicht", len);
        return;
    default:
        break;
    }
    uint8_t st = 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) != pdTRUE) {
        strlcpy(out, "beschaeftigt ...", len);
        return;
    }
    esp_err_t err = camb_reg_read(AF_FW_STATUS, &st);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        strlcpy(out, "nicht lesbar", len);
    } else if (st == 0x10 || st == 0x20) {
        strlcpy(out, "scharf gestellt", len);
    } else if (st == 0x00) {
        strlcpy(out, "stellt scharf ...", len);
    } else if (st == 0x70) {
        strlcpy(out, "Ruhe (Unendlich)", len);
    } else if (st == 0x7e || st == 0x7f) {
        strlcpy(out, "wird initialisiert ...", len);
    } else {
        snprintf(out, len, "Status 0x%02x", st);
    }
}

/* ------------------------------------------------------------------------- */
/* Allgemein                                                                  */
/* ------------------------------------------------------------------------- */

const cam_ctrl_def_t *cam_ctrl_def(cam_ctrl_id_t id)
{
    return id < CC_COUNT ? &s_defs[id] : NULL;
}

bool cam_ctrl_available(void)
{
    return s_ok;
}

const char *cam_ctrl_sensor_name(void)
{
    return s_ok ? camb_name() : "-";
}

bool cam_ctrl_supported(cam_ctrl_id_t id)
{
    return s_ok && id < CC_COUNT && s_has[id];
}

int cam_ctrl_get(cam_ctrl_id_t id)
{
    return id < CC_COUNT ? s_val[id] : 0;
}

int cam_ctrl_default(cam_ctrl_id_t id)
{
    return id < CC_COUNT ? s_def[id] : 0;
}

int cam_ctrl_find(const char *key)
{
    for (int i = 0; i < CC_COUNT; i++) {
        if (!strcmp(s_defs[i].key, key)) {
            return i;
        }
    }
    return -1;
}

static esp_err_t apply(cam_ctrl_id_t id, int value)
{
    if (id == CC_AF) {
        s_val[id] = value;
        af_request(AF_REQ_MODE);
        return ESP_OK;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = camb_set(id, value);
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        s_val[id] = value;
    }
    return err;
}

esp_err_t cam_ctrl_set(cam_ctrl_id_t id, int value)
{
    if (!cam_ctrl_supported(id)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const cam_ctrl_def_t *d = &s_defs[id];
    if (value < d->min || value > d->max) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = apply(id, value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s = %d fehlgeschlagen (%s)", d->key, value, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "%s = %d", d->key, value);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (value == s_def[id]) {
            nvs_erase_key(h, d->key);
        } else {
            nvs_set_i32(h, d->key, value);
        }
        nvs_commit(h);
        nvs_close(h);
    }
    return ESP_OK;
}

esp_err_t cam_ctrl_reset(void)
{
    if (!s_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    esp_err_t res = ESP_OK;
    for (int i = 0; i < CC_COUNT; i++) {
        if (s_has[i] && s_val[i] != s_def[i]) {
            if (apply(i, s_def[i]) != ESP_OK) {
                res = ESP_FAIL;
            }
        }
    }
    ESP_LOGI(TAG, "Kamera-Einstellungen auf Standard zurueckgesetzt");
    return res;
}

void cam_ctrl_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock || !camb_init()) {
        ESP_LOGW(TAG, "Kamera-Einstellungen nicht verfuegbar (Sensor nicht erkannt)");
        return;
    }
    s_ok = true;

    int n = 0;
    for (int i = 0; i < CC_COUNT; i++) {
        s_has[i] = camb_has(i);
        s_def[i] = s_has[i] ? camb_default(i) : 0;
        if (s_def[i] < s_defs[i].min) {
            s_def[i] = s_defs[i].min;
        } else if (s_def[i] > s_defs[i].max) {
            s_def[i] = s_defs[i].max;
        }
        s_val[i] = s_def[i];
        n += s_has[i];
    }

    /* Gespeicherte Werte anwenden (in Listenreihenfolge: erst Automatik aus, dann manuelle Werte) */
    int applied = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        for (int i = 0; i < CC_COUNT; i++) {
            int32_t v;
            if (!s_has[i] || nvs_get_i32(h, s_defs[i].key, &v) != ESP_OK) {
                continue;
            }
            if (v < s_defs[i].min || v > s_defs[i].max) {
                continue;
            }
            if (apply(i, v) == ESP_OK) {
                applied++;
            } else {
                ESP_LOGW(TAG, "Gespeicherter Wert %s = %ld nicht anwendbar", s_defs[i].key, (long)v);
            }
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Sensor %s: %d Einstellungen verfuegbar, %d gespeicherte Werte gesetzt", camb_name(), n, applied);
}

void cam_ctrl_json(char *out, size_t len)
{
    char af[48];
    cam_ctrl_af_status(af, sizeof(af));
    size_t o = snprintf(out, len, "{\"sensor\":\"%s\",\"af\":\"%s\",\"werte\":{", cam_ctrl_sensor_name(), af);
    bool first = true;
    for (int i = 0; i < CC_COUNT && o < len; i++) {
        if (!cam_ctrl_supported(i)) {
            continue;
        }
        o += snprintf(out + o, len - o, "%s\"%s\":%d", first ? "" : ",", s_defs[i].key, s_val[i]);
        first = false;
    }
    if (o < len) {
        snprintf(out + o, len - o, "}}");
    }
}
