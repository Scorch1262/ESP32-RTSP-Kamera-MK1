#pragma once

#include "esp_err.h"

/** Startet die Konfigurations-Webseite auf Port 80. */
esp_err_t web_ui_start(void);

/** Fuer die Statusanzeige: aktuelle IP (wird nach DHCP gesetzt). */
void web_ui_set_ip(const char *ip);
