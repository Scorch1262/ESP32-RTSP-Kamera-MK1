#include <string.h>
#include "app_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "config";

#define CFG_NS      "p4cam"
#define CFG_KEY     "cfg"
#define CFG_MAGIC   0x50344332u  /* "P4C2" */

typedef struct {
    uint32_t magic;
    app_config_t cfg;
} stored_cfg_t;

app_config_t g_cfg;

static void set_defaults(app_config_t *c)
{
    memset(c, 0, sizeof(*c));
    strlcpy(c->hostname, CONFIG_APP_HOSTNAME, sizeof(c->hostname));
#if CONFIG_APP_USE_STATIC_IP
    c->static_ip = true;
    strlcpy(c->ip, CONFIG_APP_STATIC_IP, sizeof(c->ip));
    strlcpy(c->netmask, CONFIG_APP_STATIC_NETMASK, sizeof(c->netmask));
    strlcpy(c->gateway, CONFIG_APP_STATIC_GW, sizeof(c->gateway));
#else
    strlcpy(c->ip, "192.168.1.60", sizeof(c->ip));
    strlcpy(c->netmask, "255.255.255.0", sizeof(c->netmask));
    strlcpy(c->gateway, "192.168.1.1", sizeof(c->gateway));
#endif
    strlcpy(c->mqtt_uri, CONFIG_APP_MQTT_URI, sizeof(c->mqtt_uri));
    strlcpy(c->mqtt_topic, CONFIG_APP_MQTT_TOPIC, sizeof(c->mqtt_topic));
    c->ir_gpio = CONFIG_APP_IR_GPIO;
#if CONFIG_IDF_TARGET_ESP32S3
    c->cam_width = CONFIG_APP_S3_CAM_WIDTH;
    c->cam_quality = CONFIG_APP_S3_JPEG_QUALITY;
#else
    c->cam_width = 1280;
    c->cam_quality = 12;
#endif
    c->wifi_mode = 0;
    c->rec_enable = true;
    c->rec_autodelete = true;
    c->rec_seconds = 20;
    c->motion_sens = 50;
    strlcpy(c->ntp_server, "pool.ntp.org", sizeof(c->ntp_server));
    c->rec_continuous = false;
    c->rec_segment_s = 300;
    c->dark_pct = 15;
    c->dark_ir_pct = 60;
    c->video_rotation = 0;
    c->stream_enable = true;
#if CONFIG_APP_IR_ACTIVE_LOW
    c->ir_active_low = true;
#endif
}

void config_load(app_config_t *cfg)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    set_defaults(cfg);

    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "Keine gespeicherte Konfiguration - Standardwerte");
        return;
    }
    /* Groesse abfragen: aeltere Versionen haben eine kuerzere Struktur gespeichert */
    size_t len = 0;
    if (nvs_get_blob(h, CFG_KEY, NULL, &len) == ESP_OK && len >= sizeof(uint32_t) + 8 &&
            len <= sizeof(stored_cfg_t)) {
        stored_cfg_t s = { .magic = 0, .cfg = *cfg };   /* neue Felder behalten Standardwerte */
        if (nvs_get_blob(h, CFG_KEY, &s, &len) == ESP_OK && s.magic == CFG_MAGIC) {
            *cfg = s.cfg;
            ESP_LOGI(TAG, "Konfiguration aus NVS geladen");
        } else {
            ESP_LOGW(TAG, "Gespeicherte Konfiguration ungueltig - Standardwerte");
        }
    } else {
        ESP_LOGW(TAG, "Gespeicherte Konfiguration hat falsches Format - Standardwerte");
    }
    nvs_close(h);
}

esp_err_t config_save(const app_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CFG_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    stored_cfg_t s = { .magic = CFG_MAGIC, .cfg = *cfg };
    err = nvs_set_blob(h, CFG_KEY, &s, sizeof(s));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
