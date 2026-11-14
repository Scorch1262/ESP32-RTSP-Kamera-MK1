#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Zur Laufzeit einstellbare Konfiguration (Webseite), gespeichert im NVS. */
typedef struct {
    char hostname[32];
    bool static_ip;
    char ip[16];
    char netmask[16];
    char gateway[16];

    char mqtt_uri[128];     /* z. B. mqtt://192.168.1.10:1883 – leer = MQTT aus */
    char mqtt_user[64];
    char mqtt_pass[64];
    char mqtt_topic[64];    /* Schalt-Topic der IR-Beleuchtung, z. B. Tuer/Kamera */

    int  ir_gpio;           /* GPIO der IR-Beleuchtung, -1 = aus */
    bool ir_active_low;     /* true: Ausgang Low = Beleuchtung ein */

    /* Ab v1.3.0 (nur ESP32-S3 genutzt) - am Ende angehaengt, aeltere Einstellungen bleiben gueltig */
    int  cam_width;         /* Aufloesung: 640, 800, 1024, 1280 (HD), 1281 (SXGA), 1600, 1920 */
    int  cam_quality;       /* JPEG-Qualitaet 4..40 (kleiner = besser) */

    /* Ab v1.4.0 - ebenfalls am Ende angehaengt */
    int  wifi_mode;         /* 0 = aus, 1 = mit WLAN verbinden, 2 = eigener Access Point (nur ESP32-S3) */
    char wifi_ssid[33];
    char wifi_pass[65];
    bool rec_enable;        /* Aufnahme auf SD-Karte bei Bewegung */
    bool rec_autodelete;    /* aelteste Aufnahmen loeschen, wenn die Karte voll ist */
    int  rec_seconds;       /* Laenge eines Bewegungs-Clips in Sekunden */
    int  motion_sens;       /* Empfindlichkeit 1..100 */
    char ntp_server[48];

    /* Ab v1.5.0 */
    bool rec_continuous;    /* Daueraufnahme als Ringspeicher */
    int  rec_segment_s;     /* Segmentlaenge der Daueraufnahme in Sekunden */
    int  dark_pct;          /* Bild gilt als dunkel unter dieser Helligkeit (%) */
    int  dark_ir_pct;       /* bei eingeschaltetem IR-Licht erst ueber dieser Helligkeit wieder "hell" (%) */

    /* Ab v1.7.0 */
    int  video_rotation;    /* Bilddrehung im Uhrzeigersinn: 0, 90, 180, 270 */
} app_config_t;

/** Laedt die Konfiguration aus dem NVS (oder Standardwerte). */
void config_load(app_config_t *cfg);

/** Speichert die Konfiguration im NVS. */
esp_err_t config_save(const app_config_t *cfg);

/** Globale, beim Start geladene Konfiguration. */
extern app_config_t g_cfg;
