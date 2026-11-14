#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

/** Startet die interne EMAC + PHY des ESP32-P4-ETH (nicht blockierend). */
esp_err_t eth_start(void);

/** Wartet, bis eine IP-Adresse vorliegt. Schreibt sie als Text nach ip_out (min. 16 Byte). */
bool eth_wait_for_ip(TickType_t timeout, char *ip_out, size_t ip_out_len);
