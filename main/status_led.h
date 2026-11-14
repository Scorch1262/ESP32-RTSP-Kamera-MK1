#pragma once

#include <stdint.h>
#include "esp_err.h"

/** RGB-Status-LED (WS2812, z. B. GPIO21 am ESP32-S3-ETH). gpio < 0 = aus. */
esp_err_t status_led_init(int gpio);

void status_led_set(uint8_t r, uint8_t g, uint8_t b);
