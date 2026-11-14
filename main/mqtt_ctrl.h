#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "app_config.h"

/**
 * Verbindet sich mit dem MQTT-Broker (falls konfiguriert).
 *   <topic>             <- 0 / 1  (IR-Beleuchtung aus / ein)
 *   <topic>/Status      -> 0 / 1  (aktueller Zustand, retained)
 *   <topic>/Verfuegbar  -> online / offline (retained, Last Will)
 *   <topic>/Info        -> Geraeteinfo inkl. RTSP-URL (retained)
 *   <topic>/Bewegung    -> 1 / 0 (Bewegung erkannt / vorbei)
 *   <topic>/Aufnahme    -> Dateiname einer neuen Aufnahme
 */
esp_err_t mqtt_ctrl_start(const app_config_t *cfg, const char *ip);

bool mqtt_ctrl_connected(void);

/** Veroeffentlicht <topic>/<suffix> (wenn verbunden). */
void mqtt_ctrl_publish(const char *suffix, const char *payload, bool retain);

/** Schaltet lokal (z. B. von der Webseite) und gleicht den Broker ab. */
void mqtt_ctrl_set_light(bool on);
