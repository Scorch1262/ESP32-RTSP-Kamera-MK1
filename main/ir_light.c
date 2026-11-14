/*
 * Schaltausgang fuer eine IR-Beleuchtung (IR-LED / IR-Strahler ueber Transistor/MOSFET).
 */
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "ir_light.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "ir_light";

static int s_gpio = -1;
static bool s_active_low;
static volatile bool s_on;

esp_err_t ir_light_init(int gpio, bool active_low)
{
    s_gpio = gpio;
    s_active_low = active_low;
    if (gpio < 0) {
        ESP_LOGI(TAG, "IR-Beleuchtung deaktiviert (kein GPIO)");
        return ESP_OK;
    }
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        s_gpio = -1;
        return err;
    }
    ir_light_set(false);
    ESP_LOGI(TAG, "IR-Beleuchtung an GPIO %d (%s)", gpio, active_low ? "Low-aktiv" : "High-aktiv");
    return ESP_OK;
}

void ir_light_set(bool on)
{
    s_on = on;
    if (s_gpio >= 0) {
        gpio_set_level(s_gpio, (on ^ s_active_low) ? 1 : 0);
    }
    ESP_LOGI(TAG, "IR-Beleuchtung %s", on ? "EIN" : "AUS");
}

bool ir_light_get(void)
{
    return s_on;
}

bool ir_light_available(void)
{
    return s_gpio >= 0;
}

bool ir_light_apply_payload(const char *payload, int len)
{
    char v[16];
    int o = 0;
    for (int i = 0; i < len && o < (int)sizeof(v) - 1; i++) {
        if (!isspace((unsigned char)payload[i]) && payload[i] != '"') {
            v[o++] = payload[i];
        }
    }
    v[o] = 0;

    if (!strcmp(v, "1") || !strcasecmp(v, "on") || !strcasecmp(v, "ein") || !strcasecmp(v, "true")) {
        ir_light_set(true);
    } else if (!strcmp(v, "0") || !strcasecmp(v, "off") || !strcasecmp(v, "aus") || !strcasecmp(v, "false")) {
        ir_light_set(false);
    } else if (!strcasecmp(v, "toggle") || !strcasecmp(v, "umschalten")) {
        ir_light_set(!s_on);
    } else {
        ESP_LOGW(TAG, "Unbekannte Nutzlast \"%s\" - erwartet 0/1", v);
        return false;
    }
    return true;
}
