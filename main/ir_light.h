#pragma once

#include <stdbool.h>
#include "esp_err.h"

/** Initialisiert den Schaltausgang der IR-Beleuchtung (gpio < 0 = aus). */
esp_err_t ir_light_init(int gpio, bool active_low);

/** Schaltet die IR-Beleuchtung ein/aus. */
void ir_light_set(bool on);

bool ir_light_get(void);

/** true, wenn ein GPIO konfiguriert ist. */
bool ir_light_available(void);

/**
 * Wertet eine Nutzlast aus: "1", "0", "on", "off", "ein", "aus", "true", "false",
 * "toggle"/"umschalten" (Gross-/Kleinschreibung egal).
 * @return true bei gueltiger Nutzlast
 */
bool ir_light_apply_payload(const char *payload, int len);
