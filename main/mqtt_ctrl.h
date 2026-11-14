#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "app_config.h"

/**
 * Verbindet sich mit dem MQTT-Broker (falls konfiguriert).
 *   <topic>/ir/send    <- JSON-IR-Befehl
 *   <topic>/ir/result  -> Rueckmeldung (JSON)
 *   <topic>/status     -> "online" / "offline" (retained, Last Will)
 *   <topic>/info       -> Geraeteinfo inkl. RTSP-URL (retained)
 */
esp_err_t mqtt_ctrl_start(const app_config_t *cfg, const char *ip);

bool mqtt_ctrl_connected(void);
