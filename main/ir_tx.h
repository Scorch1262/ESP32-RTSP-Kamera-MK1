#pragma once

#include <stddef.h>
#include "esp_err.h"

/** Initialisiert den RMT-Sender an gpio (Traeger wird pro Befehl gesetzt). */
esp_err_t ir_tx_init(int gpio);

/**
 * Fuehrt einen IR-Befehl im JSON-Format aus (blockierend), z. B.
 *   {"protocol":"NEC","address":4,"command":8,"repeat":0}
 *   {"protocol":"SAMSUNG","address":"0x07","command":"0x02"}
 *   {"protocol":"SONY","address":1,"command":21,"bits":12}
 *   {"protocol":"RC5","address":0,"command":12,"toggle":0}
 *   {"raw":[9000,4500,560,560,...],"freq":38000}
 *   {"pronto":"0000 006D 0022 0002 0157 00AC ..."}
 * In result (falls != NULL) landet eine kurze Rueckmeldung.
 */
esp_err_t ir_tx_send_json(const char *json, char *result, size_t result_len);

/** Anzahl erfolgreich gesendeter Befehle seit dem Start. */
unsigned ir_tx_count(void);
