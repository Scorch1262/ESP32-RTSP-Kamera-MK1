/*
 * WLAN des ESP32-S3: Client (mit vorhandenem WLAN verbinden) oder eigener Access Point.
 * Laeuft parallel zum Ethernet; Webseite, RTSP und MQTT sind ueber beide Wege erreichbar.
 * Das ESP32-P4-ETH hat kein WLAN - dort sind nur Platzhalter aktiv.
 */
#include <string.h>
#include <stdio.h>
#include "wifi_ctrl.h"
#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32S3

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "lwip/ip4_addr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const char *TAG = "wlan";

static int s_mode = APP_WIFI_OFF;
static bool s_inited;
static bool s_started;
static char s_ip[16];
static char s_state[96] = "aus";
static esp_netif_t *s_sta, *s_ap;
static int s_retry;
static esp_timer_handle_t s_reconnect;

static void reconnect_cb(void *arg)
{
    esp_wifi_connect();
}

bool wifi_ctrl_available(void)
{
    return true;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            if (s_mode == APP_WIFI_STA) {
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *d = data;
            s_ip[0] = 0;
            if (s_mode == APP_WIFI_STA) {
                snprintf(s_state, sizeof(s_state), "getrennt (Grund %d), neuer Versuch ...", d->reason);
                /* nicht im Event-Task warten - Neuversuch per Timer */
                int delay = s_retry < 5 ? 2 : 15;
                s_retry++;
                esp_timer_stop(s_reconnect);
                esp_timer_start_once(s_reconnect, (uint64_t)delay * 1000000);
            }
            break;
        }
        case WIFI_EVENT_STA_CONNECTED:
            snprintf(s_state, sizeof(s_state), "verbunden, warte auf IP-Adresse");
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "Geraet mit dem Access Point verbunden");
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        wifi_ap_record_t ap;
        int rssi = 0;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            rssi = ap.rssi;
        }
        snprintf(s_state, sizeof(s_state), "verbunden, IP %s, Signal %d dBm", s_ip, rssi);
        s_retry = 0;
        ESP_LOGI(TAG, "WLAN-IP-Adresse: %s", s_ip);
    }
}

static esp_err_t ensure_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_sta = esp_netif_create_default_wifi_sta();
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&ic);
    if (err != ESP_OK) {
        return err;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    const esp_timer_create_args_t ta = { .callback = reconnect_cb, .name = "wlan_retry" };
    esp_timer_create(&ta, &s_reconnect);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    s_inited = true;
    return ESP_OK;
}

esp_err_t wifi_ctrl_start(const app_config_t *cfg)
{
    s_mode = cfg->wifi_mode;
    if (s_mode == APP_WIFI_OFF) {
        snprintf(s_state, sizeof(s_state), "aus");
        return ESP_OK;
    }
    esp_err_t err = ensure_init();
    if (err != ESP_OK) {
        snprintf(s_state, sizeof(s_state), "Fehler: %s", esp_err_to_name(err));
        return err;
    }
    esp_netif_set_hostname(s_sta, cfg->hostname);

    if (s_mode == APP_WIFI_STA) {
        if (!cfg->wifi_ssid[0]) {
            snprintf(s_state, sizeof(s_state), "kein WLAN-Name (SSID) eingetragen");
            return ESP_ERR_INVALID_ARG;
        }
        wifi_config_t wc = {0};
        strlcpy((char *)wc.sta.ssid, cfg->wifi_ssid, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, cfg->wifi_pass, sizeof(wc.sta.password));
        wc.sta.threshold.authmode = cfg->wifi_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
        wc.sta.pmf_cfg.capable = true;
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        snprintf(s_state, sizeof(s_state), "verbinde mit \"%s\" ...", cfg->wifi_ssid);
        ESP_LOGI(TAG, "Verbinde mit WLAN \"%s\"", cfg->wifi_ssid);
    } else {
        s_ap = esp_netif_create_default_wifi_ap();
        wifi_config_t wc = {0};
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        if (cfg->wifi_ssid[0]) {
            strlcpy((char *)wc.ap.ssid, cfg->wifi_ssid, sizeof(wc.ap.ssid));
        } else {
            snprintf((char *)wc.ap.ssid, sizeof(wc.ap.ssid), "P4-RTSP-Cam-%02X%02X", mac[4], mac[5]);
        }
        wc.ap.ssid_len = strlen((char *)wc.ap.ssid);
        bool secure = strlen(cfg->wifi_pass) >= 8;
        if (secure) {
            strlcpy((char *)wc.ap.password, cfg->wifi_pass, sizeof(wc.ap.password));
        }
        wc.ap.authmode = secure ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        wc.ap.channel = 6;
        wc.ap.max_connection = 4;
        esp_wifi_set_mode(WIFI_MODE_APSTA);   /* APSTA: WLAN-Suche bleibt moeglich */
        esp_wifi_set_config(WIFI_IF_AP, &wc);
        strlcpy(s_ip, "192.168.4.1", sizeof(s_ip));
        snprintf(s_state, sizeof(s_state), "Access Point \"%s\" (%s), IP 192.168.4.1",
                 (char *)wc.ap.ssid, secure ? "WPA2" : "offen");
        ESP_LOGI(TAG, "%s", s_state);
    }
    err = esp_wifi_start();
    s_started = (err == ESP_OK);
    return err;
}

const char *wifi_ctrl_ip(void)
{
    return s_ip[0] ? s_ip : NULL;
}

const char *wifi_ctrl_state(void)
{
    return s_state;
}

esp_err_t wifi_ctrl_scan_json(char *out, size_t len)
{
    esp_err_t err = ensure_init();
    if (err != ESP_OK) {
        return err;
    }
    bool temp = false;
    if (!s_started) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        if (esp_wifi_start() != ESP_OK) {
            return ESP_FAIL;
        }
        temp = true;
    }
    wifi_scan_config_t sc = { .show_hidden = false };
    err = esp_wifi_scan_start(&sc, true);
    uint16_t n = 20;
    wifi_ap_record_t recs[20];
    if (err == ESP_OK) {
        esp_wifi_scan_get_ap_records(&n, recs);
    } else {
        n = 0;
    }
    if (temp) {
        esp_wifi_stop();
    }
    size_t o = snprintf(out, len, "[");
    for (int i = 0; i < n && o + 80 < len; i++) {
        char ssid[66];
        size_t k = 0;
        for (const char *p = (const char *)recs[i].ssid; *p && k + 2 < sizeof(ssid); p++) {
            if (*p == '"' || *p == '\\') {
                ssid[k++] = '\\';
            }
            ssid[k++] = *p;
        }
        ssid[k] = 0;
        if (!k) {
            continue;
        }
        o += snprintf(out + o, len - o, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}", o > 1 ? "," : "",
                      ssid, recs[i].rssi, recs[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
    }
    snprintf(out + o, len - o, "]");
    return err;
}

#else  /* kein WLAN auf diesem Board */

bool wifi_ctrl_available(void)
{
    return false;
}

esp_err_t wifi_ctrl_start(const app_config_t *cfg)
{
    return ESP_OK;
}

const char *wifi_ctrl_ip(void)
{
    return NULL;
}

const char *wifi_ctrl_state(void)
{
    return "nicht vorhanden (ESP32-P4-ETH hat kein WLAN)";
}

esp_err_t wifi_ctrl_scan_json(char *out, size_t len)
{
    snprintf(out, len, "[]");
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
