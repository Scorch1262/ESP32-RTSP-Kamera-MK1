#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "app_config.h"

/**
 * Verbindet sich mit dem MQTT-Broker (falls konfiguriert). Zustand und Steuerung
 * laufen jeweils im selben Topic (siehe mqtt_ctrl.c), alles ohne Retain-Flag.
 */
esp_err_t mqtt_ctrl_start(const app_config_t *cfg, const char *ip);

bool mqtt_ctrl_connected(void);

/** Veroeffentlicht <Basis>/<suffix> (wenn verbunden, nicht persistent). */
void mqtt_ctrl_publish(const char *suffix, const char *payload);

/** Schaltet lokal (z. B. von der Webseite) und gleicht den Broker ab. */
void mqtt_ctrl_set_light(bool on);
