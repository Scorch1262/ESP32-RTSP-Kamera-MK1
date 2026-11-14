#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "app_config.h"

typedef enum {
    ETH_STATE_ERROR = 0,   /* Treiber/Chip nicht gefunden */
    ETH_STATE_NO_LINK,     /* kein Kabel / kein Link */
    ETH_STATE_LINK_UP,     /* Link da, warte auf IP */
    ETH_STATE_GOT_IP,
} eth_state_t;

eth_state_t eth_get_state(void);

/** Aktuelle IP als Text oder NULL. */
const char *eth_get_ip(void);

/** Startet Ethernet (P4: EMAC + IP101, S3: W5500), nicht blockierend. */
esp_err_t eth_start(const app_config_t *cfg);

/** Wartet, bis eine IP-Adresse vorliegt. Schreibt sie als Text nach ip_out (min. 16 Byte). */
bool eth_wait_for_ip(TickType_t timeout, char *ip_out, size_t ip_out_len);
