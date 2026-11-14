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
    char mqtt_topic[64];    /* Basis-Topic, z. B. p4cam */

    int  ir_gpio;           /* GPIO der IR-LED, -1 = IR aus */
} app_config_t;

/** Laedt die Konfiguration aus dem NVS (oder Standardwerte). */
void config_load(app_config_t *cfg);

/** Speichert die Konfiguration im NVS. */
esp_err_t config_save(const app_config_t *cfg);

/** Globale, beim Start geladene Konfiguration. */
extern app_config_t g_cfg;
