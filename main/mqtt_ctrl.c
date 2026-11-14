#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "mqtt_ctrl.h"
#include "ir_tx.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "cJSON.h"
#include "sdkconfig.h"

static const char *TAG = "mqtt";

#define MAX_PAYLOAD 8192

static esp_mqtt_client_handle_t s_client;
static QueueHandle_t s_queue;
static volatile bool s_connected;
static char s_base[64];
static char s_ip[16];

static char *s_rx;            /* fuer fragmentierte Nachrichten */
static int s_rx_len;

static void topic(char *out, size_t len, const char *suffix)
{
    snprintf(out, len, "%s/%s", s_base, suffix);
}

static void publish_info(void)
{
    char t[96], payload[320];
    snprintf(payload, sizeof(payload),
             "{\"device\":\"P4-ETH-RTSP-Cam-MK1\",\"version\":\"%s\",\"ip\":\"%s\","
             "\"rtsp\":\"rtsp://%s:%d/%s\",\"ir_topic\":\"%s/ir/send\"}",
             esp_app_get_description()->version, s_ip, s_ip, CONFIG_APP_RTSP_PORT,
             CONFIG_APP_RTSP_PATH, s_base);
    topic(t, sizeof(t), "info");
    esp_mqtt_client_publish(s_client, t, payload, 0, 1, 1);
}

static void handle_message(const char *data, int len)
{
    char *copy = malloc(len + 1);
    if (!copy) {
        return;
    }
    memcpy(copy, data, len);
    copy[len] = 0;
    if (xQueueSend(s_queue, &copy, 0) != pdTRUE) {
        ESP_LOGW(TAG, "IR-Warteschlange voll - Befehl verworfen");
        free(copy);
    }
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    char t[96];

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "Mit Broker verbunden");
        topic(t, sizeof(t), "ir/send");
        esp_mqtt_client_subscribe(s_client, t, 1);
        topic(t, sizeof(t), "status");
        esp_mqtt_client_publish(s_client, t, "online", 0, 1, 1);
        publish_info();
        ESP_LOGI(TAG, "Warte auf IR-Befehle unter %s/ir/send", s_base);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "Verbindung zum Broker getrennt");
        break;
    case MQTT_EVENT_DATA:
        if (ev->total_data_len > MAX_PAYLOAD) {
            ESP_LOGW(TAG, "Nachricht zu gross (%d Byte)", ev->total_data_len);
            break;
        }
        if (ev->current_data_offset == 0) {
            s_rx_len = 0;
        }
        if (ev->data_len == ev->total_data_len) {
            handle_message(ev->data, ev->data_len);
            break;
        }
        if (!s_rx) {
            s_rx = malloc(MAX_PAYLOAD);
        }
        if (s_rx && s_rx_len + ev->data_len <= MAX_PAYLOAD) {
            memcpy(s_rx + s_rx_len, ev->data, ev->data_len);
            s_rx_len += ev->data_len;
            if (s_rx_len == ev->total_data_len) {
                handle_message(s_rx, s_rx_len);
                s_rx_len = 0;
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

static void ir_worker(void *arg)
{
    char t[96];
    char result[160];
    for (;;) {
        char *msg;
        xQueueReceive(s_queue, &msg, portMAX_DELAY);
        esp_err_t err = ir_tx_send_json(msg, result, sizeof(result));
        free(msg);

        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", err == ESP_OK);
        cJSON_AddStringToObject(r, "message", result);
        char *out = cJSON_PrintUnformatted(r);
        cJSON_Delete(r);
        if (out && s_connected) {
            topic(t, sizeof(t), "ir/result");
            esp_mqtt_client_publish(s_client, t, out, 0, 0, 0);
        }
        free(out);
    }
}

esp_err_t mqtt_ctrl_start(const app_config_t *cfg, const char *ip)
{
    if (cfg->mqtt_uri[0] == 0) {
        ESP_LOGI(TAG, "Kein Broker konfiguriert - MQTT aus");
        return ESP_OK;
    }
    strlcpy(s_base, cfg->mqtt_topic[0] ? cfg->mqtt_topic : "p4cam", sizeof(s_base));
    strlcpy(s_ip, ip, sizeof(s_ip));

    s_queue = xQueueCreate(8, sizeof(char *));
    xTaskCreate(ir_worker, "ir_worker", 6144, NULL, 5, NULL);

    static char lwt_topic[96];
    topic(lwt_topic, sizeof(lwt_topic), "status");

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
