#include <string.h>
#include <stdio.h>
#include "mqtt_ctrl.h"
#include "ir_light.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "sdkconfig.h"

static const char *TAG = "mqtt";

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static char s_topic[64];
static char s_ip[16];

static void sub_topic(char *out, size_t len, const char *suffix)
{
    snprintf(out, len, "%s/%s", s_topic, suffix);
}

static void publish_state(void)
{
    if (!s_connected) {
        return;
    }
    char t[96];
    sub_topic(t, sizeof(t), "Status");
    esp_mqtt_client_publish(s_client, t, ir_light_get() ? "1" : "0", 1, 1, 1);
}

static void publish_info(void)
{
    char t[96], payload[320];
    snprintf(payload, sizeof(payload),
             "{\"device\":\"P4-ETH-RTSP-Cam-MK1\",\"version\":\"%s\",\"ip\":\"%s\","
             "\"rtsp\":\"rtsp://%s:%d/%s\",\"ir_topic\":\"%s\"}",
             esp_app_get_description()->version, s_ip, s_ip, CONFIG_APP_RTSP_PORT,
             CONFIG_APP_RTSP_PATH, s_topic);
    sub_topic(t, sizeof(t), "Info");
    esp_mqtt_client_publish(s_client, t, payload, 0, 1, 1);
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    char t[96];

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "Mit Broker verbunden - Schalt-Topic: %s (0/1)", s_topic);
        esp_mqtt_client_subscribe(s_client, s_topic, 1);
        sub_topic(t, sizeof(t), "Verfuegbar");
        esp_mqtt_client_publish(s_client, t, "online", 0, 1, 1);
        publish_info();
        publish_state();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "Verbindung zum Broker getrennt");
        break;
    case MQTT_EVENT_DATA:
        if (ev->topic_len == (int)strlen(s_topic) && memcmp(ev->topic, s_topic, ev->topic_len) == 0 &&
                ev->current_data_offset == 0 && ev->data_len <= 32) {
            if (ir_light_apply_payload(ev->data, ev->data_len)) {
                publish_state();
            }
        }
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT-Fehler");
        break;
    default:
        break;
    }
}

void mqtt_ctrl_set_light(bool on)
{
    ir_light_set(on);
    if (s_connected) {
        /* Schalt-Topic retained mitziehen, damit der Broker den Zustand kennt */
        esp_mqtt_client_publish(s_client, s_topic, on ? "1" : "0", 1, 1, 1);
        publish_state();
    }
}

esp_err_t mqtt_ctrl_start(const app_config_t *cfg, const char *ip)
{
    if (cfg->mqtt_uri[0] == 0) {
        ESP_LOGI(TAG, "Kein Broker konfiguriert - MQTT aus");
        return ESP_OK;
    }
    strlcpy(s_topic, cfg->mqtt_topic[0] ? cfg->mqtt_topic : "Tuer/Kamera", sizeof(s_topic));
    strlcpy(s_ip, ip, sizeof(s_ip));

    static char lwt_topic[96];
    sub_topic(lwt_topic, sizeof(lwt_topic), "Verfuegbar");

    esp_mqtt_client_config_t mc = {
        .broker.address.uri = cfg->mqtt_uri,
        .credentials.client_id = cfg->hostname,
        .session.last_will = {
            .topic = lwt_topic,
            .msg = "offline",
            .qos = 1,
            .retain = 1,
        },
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 5000,
    };
    if (cfg->mqtt_user[0]) {
        mc.credentials.username = cfg->mqtt_user;
        mc.credentials.authentication.password = cfg->mqtt_pass;
    }
    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) {
        return ESP_FAIL;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    ESP_LOGI(TAG, "Verbinde mit %s ...", cfg->mqtt_uri);
    return esp_mqtt_client_start(s_client);
}

bool mqtt_ctrl_connected(void)
{
    return s_connected;
}
