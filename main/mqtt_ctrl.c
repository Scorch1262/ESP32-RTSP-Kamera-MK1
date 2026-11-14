/*
 * MQTT: Zustand und Steuerung im selben Topic.
 *
 *   <Basis>/IR-Licht          0/1   IR-Licht aus/ein
 *   <Basis>/Daueraufnahme     0/1   Daueraufnahme (Ringspeicher) aus/ein
 *   <Basis>/Aufzeichnung      0/1   Zustand: Aufzeichnung laeuft; 1 = Clip starten, 0 = beenden
 *   <Basis>/Bewegung          0/1   Zustand: Bewegung erkannt; 1 = Bewegung ausloesen, 0 = zuruecksetzen
 *   <Basis>/Bewegungsaufnahme 0/1   Aufnahme bei Bewegung aus/ein
 *   <Basis>/Empfindlichkeit   1-100 Empfindlichkeit der Bewegungserkennung
 *   <Basis>/Dunkel, /Helligkeit, /Aufnahme, /Verfuegbar, /Info: nur Meldungen (Messwerte)
 *
 * Das Board abonniert die steuerbaren Topics und empfaengt dadurch auch seine eigenen
 * Meldungen. Eine eingehende Nachricht, die genau dem zuletzt selbst gesendeten Wert
 * entspricht, ist dieses Echo (bzw. aendert nichts) und wird ignoriert.
 * Alle Nachrichten werden ohne Retain-Flag gesendet (nicht persistent).
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <ctype.h>
#include "mqtt_ctrl.h"
#include "ir_light.h"
#include "recorder.h"
#include "motion.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

static const char *TAG = "mqtt";

void app_request_state_publish(void);   /* main.c: alle Zustaende kurz danach erneut melden */

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static char s_topic[64];
static char s_ip[16];

/* Steuerbare Topics */
static const char *const s_ctrl[] = {
    "IR-Licht", "Daueraufnahme", "Aufzeichnung", "Bewegung", "Bewegungsaufnahme", "Empfindlichkeit",
};
#define N_CTRL (sizeof(s_ctrl) / sizeof(s_ctrl[0]))

/*
 * Echo-Erkennung: Jede eigene Meldung kommt genau einmal vom Broker zurueck.
 * Sie wird als "erwartetes Echo" vermerkt und beim Eintreffen abgehakt - auch wenn
 * sie verspaetet ankommt. Eine Nachricht, die kein erwartetes Echo ist, stammt von
 * aussen: entspricht sie dem aktuellen Zustand, aendert sie nichts, sonst ist sie
 * ein Befehl.
 */
#define N_ECHO   32
#define ECHO_US  (10LL * 1000 * 1000)
typedef struct {
    char suffix[24];
    char value[16];
    int64_t t;
    bool pending;            /* Echo steht noch aus */
} echo_t;
static echo_t s_echo[N_ECHO];
static int s_echo_pos;
static SemaphoreHandle_t s_echo_lock;

static void sub_topic(char *out, size_t len, const char *suffix)
{
    snprintf(out, len, "%s/%s", s_topic, suffix);
}

static void echo_store(const char *suffix, const char *value)
{
    if (!s_echo_lock) {
        return;
    }
    xSemaphoreTake(s_echo_lock, portMAX_DELAY);
    echo_t *e = &s_echo[s_echo_pos];
    s_echo_pos = (s_echo_pos + 1) % N_ECHO;
    strlcpy(e->suffix, suffix, sizeof(e->suffix));
    strlcpy(e->value, value, sizeof(e->value));
    e->t = esp_timer_get_time();
    e->pending = true;
    xSemaphoreGive(s_echo_lock);
}

/* true = ignorieren (eigenes Echo oder keine Aenderung) */
static bool echo_matches(const char *suffix, const char *value)
{
    int64_t now = esp_timer_get_time();
    echo_t *oldest_pending = NULL, *newest = NULL;
    xSemaphoreTake(s_echo_lock, portMAX_DELAY);
    for (int i = 0; i < N_ECHO; i++) {
        echo_t *e = &s_echo[i];
        if (!e->suffix[0] || strcmp(e->suffix, suffix) != 0) {
            continue;
        }
        if (e->pending && now - e->t < ECHO_US && strcmp(e->value, value) == 0 &&
                (!oldest_pending || e->t < oldest_pending->t)) {
            oldest_pending = e;
        }
        if (!newest || e->t > newest->t) {
            newest = e;
        }
    }
    bool ignore;
    if (oldest_pending) {
        oldest_pending->pending = false;    /* Echo abgehakt */
        ignore = true;
    } else {
        ignore = newest && strcmp(newest->value, value) == 0;
    }
    xSemaphoreGive(s_echo_lock);
    return ignore;
}

static void echo_reset(void)
{
    xSemaphoreTake(s_echo_lock, portMAX_DELAY);
    memset(s_echo, 0, sizeof(s_echo));
    s_echo_pos = 0;
    xSemaphoreGive(s_echo_lock);
}

void mqtt_ctrl_publish(const char *suffix, const char *payload)
{
    if (!s_connected) {
        return;
    }
    char t[128];
    sub_topic(t, sizeof(t), suffix);
    echo_store(suffix, payload);
    esp_mqtt_client_publish(s_client, t, payload, 0, 1, 0);
}

static void publish_info(void)
{
    char payload[320];
    snprintf(payload, sizeof(payload),
             "{\"device\":\"P4-ETH-RTSP-Cam-MK1\",\"version\":\"%s\",\"ip\":\"%s\","
             "\"rtsp\":\"rtsp://%s:%d/%s\",\"topic\":\"%s\"}",
             esp_app_get_description()->version, s_ip, s_ip, CONFIG_APP_RTSP_PORT,
             CONFIG_APP_RTSP_PATH, s_topic);
    mqtt_ctrl_publish("Info", payload);
}

/*
 * Bis v1.5.0 wurden einige Meldungen "retained" (persistent) gesendet. Einmalig je Geraet
 * werden sie durch eine leere retained-Nachricht vom Broker geloescht.
 */
static void clear_old_retained(void)
{
    nvs_handle_t h;
    uint8_t done = 0;
    if (nvs_open("p4cam", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_get_u8(h, "mqtt_clean", &done);
    if (!done) {
        static const char *suffixes[] = { "Status", "Verfuegbar", "Info", "Bewegung", "Aufzeichnung",
                                          "Daueraufnahme", "Dunkel", "Helligkeit" };
        char t[128];
        for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
            sub_topic(t, sizeof(t), suffixes[i]);
            esp_mqtt_client_publish(s_client, t, "", 0, 1, 1);
        }
        esp_mqtt_client_publish(s_client, s_topic, "", 0, 1, 1);
        nvs_set_u8(h, "mqtt_clean", 1);
        nvs_commit(h);
        ESP_LOGI(TAG, "Alte persistente (retained) Meldungen auf dem Broker geloescht");
    }
    nvs_close(h);
}

/* ------------------------------------------------------------------------- */
/* Befehle                                                                    */
/* ------------------------------------------------------------------------- */

/* "1"/"on"/"ein"/"true" -> 1, "0"/"off"/"aus"/"false" -> 0, sonst -1 */
static int parse_bool(const char *v)
{
    if (!strcmp(v, "1") || !strcasecmp(v, "on") || !strcasecmp(v, "ein") || !strcasecmp(v, "true")) {
        return 1;
    }
    if (!strcmp(v, "0") || !strcasecmp(v, "off") || !strcasecmp(v, "aus") || !strcasecmp(v, "false")) {
        return 0;
    }
    return -1;
}

static void handle_command(const char *suffix, const char *v)
{
    int b = parse_bool(v);
    bool known = true;

    if (!strcmp(suffix, "IR-Licht")) {
        if (!strcasecmp(v, "toggle") || !strcasecmp(v, "umschalten")) {
            b = !ir_light_get();
        }
        if (b >= 0) {
            ir_light_set(b);
        }
    } else if (!strcmp(suffix, "Daueraufnahme")) {
        if (b >= 0 && recorder_set_continuous(b) != ESP_OK) {
            ESP_LOGW(TAG, "Daueraufnahme nicht moeglich (keine SD-Karte?)");
        }
    } else if (!strcmp(suffix, "Aufzeichnung")) {
        if (b == 1) {
            if (recorder_start_manual() != ESP_OK) {
                ESP_LOGW(TAG, "Aufzeichnung nicht moeglich (keine SD-Karte?)");
            }
        } else if (b == 0) {
            if (recorder_continuous()) {
                recorder_set_continuous(false);
            }
            recorder_stop();
        }
    } else if (!strcmp(suffix, "Bewegung")) {
        if (b == 1) {
            motion_trigger();
            recorder_motion();
        } else if (b == 0) {
            motion_clear();
        }
    } else if (!strcmp(suffix, "Bewegungsaufnahme")) {
        if (b >= 0 && g_cfg.rec_enable != (bool)b) {
            g_cfg.rec_enable = b;
            config_save(&g_cfg);
        }
    } else if (!strcmp(suffix, "Empfindlichkeit")) {
        char *end;
        long n = strtol(v, &end, 10);
        if (end != v && *end == 0 && n >= 1 && n <= 100) {
            if (g_cfg.motion_sens != n) {
                g_cfg.motion_sens = (int)n;
                config_save(&g_cfg);
            }
            b = 1;
        } else {
            b = -1;
        }
    } else {
        known = false;
    }

    if (known) {
        if (b < 0) {
            ESP_LOGW(TAG, "%s: ungueltiger Wert \"%s\"", suffix, v);
        } else {
            ESP_LOGI(TAG, "Befehl %s = %s", suffix, v);
        }
        /* Tatsaechlichen Zustand zuruecksenden (auch wenn der Befehl nicht ausfuehrbar war) */
        app_request_state_publish();
    }
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    char t[128];

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        echo_reset();
        ESP_LOGI(TAG, "Mit Broker verbunden - Basis-Topic: %s", s_topic);
        clear_old_retained();
        for (size_t i = 0; i < N_CTRL; i++) {
            sub_topic(t, sizeof(t), s_ctrl[i]);
            esp_mqtt_client_subscribe(s_client, t, 1);
        }
        mqtt_ctrl_publish("Verfuegbar", "online");
        publish_info();
        app_request_state_publish();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "Verbindung zum Broker getrennt");
        break;
    case MQTT_EVENT_DATA: {
        size_t bl = strlen(s_topic);
        if (ev->current_data_offset != 0 || ev->data_len <= 0 || ev->data_len > 15 ||
                ev->topic_len <= (int)bl + 1 || ev->topic_len - (int)bl - 1 >= 24 ||
                memcmp(ev->topic, s_topic, bl) != 0 || ev->topic[bl] != '/') {
            break;
        }
        char suffix[24], val[16];
        memcpy(suffix, ev->topic + bl + 1, ev->topic_len - bl - 1);
        suffix[ev->topic_len - bl - 1] = 0;
        int o = 0;
        for (int i = 0; i < ev->data_len && o < (int)sizeof(val) - 1; i++) {
            if (!isspace((unsigned char)ev->data[i]) && ev->data[i] != '"') {
                val[o++] = ev->data[i];
            }
        }
        val[o] = 0;
        if (!val[0] || echo_matches(suffix, val)) {
            break;                      /* eigenes Echo oder keine Aenderung */
        }
        handle_command(suffix, val);
        break;
    }
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
    ESP_LOGI(TAG, "IR-Licht ueber Webseite %s", on ? "EIN" : "AUS");
    mqtt_ctrl_publish("IR-Licht", on ? "1" : "0");   /* ohne Verbindung: Meldung nach Neuverbindung */
}

esp_err_t mqtt_ctrl_start(const app_config_t *cfg, const char *ip)
{
    if (cfg->mqtt_uri[0] == 0) {
        ESP_LOGI(TAG, "Kein Broker konfiguriert - MQTT aus");
        return ESP_OK;
    }
    strlcpy(s_topic, cfg->mqtt_topic[0] ? cfg->mqtt_topic : "Tuer/Kamera", sizeof(s_topic));
    strlcpy(s_ip, ip, sizeof(s_ip));
    s_echo_lock = xSemaphoreCreateMutex();

    static char lwt_topic[96];
    sub_topic(lwt_topic, sizeof(lwt_topic), "Verfuegbar");

    esp_mqtt_client_config_t mc = {
        .broker.address.uri = cfg->mqtt_uri,
        .credentials.client_id = cfg->hostname,
        .session.last_will = {
            .topic = lwt_topic,
            .msg = "offline",
            .qos = 1,
            .retain = 0,          /* keine persistenten Nachrichten */
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
