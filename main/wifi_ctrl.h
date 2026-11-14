#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "app_config.h"

#define APP_WIFI_OFF 0
#define APP_WIFI_STA 1
#define APP_WIFI_AP  2

/** true, wenn das Board WLAN hat (ESP32-S3). */
bool wifi_ctrl_available(void);

/** Startet WLAN gemaess Konfiguration (Client oder Access Point). */
esp_err_t wifi_ctrl_start(const app_config_t *cfg);

/** IP-Adresse im WLAN als Text oder NULL. */
const char *wifi_ctrl_ip(void);

/** Kurzer Zustandstext fuer Webseite/Konsole. */
const char *wifi_ctrl_state(void);

/** Sucht WLANs und schreibt ein JSON-Array [{"ssid":..,"rssi":..,"auth":..},..]. */
esp_err_t wifi_ctrl_scan_json(char *out, size_t len);
