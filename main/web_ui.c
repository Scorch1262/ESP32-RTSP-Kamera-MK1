/*
 * Konfigurations-Webseite: Netzwerk, MQTT, IR-Beleuchtung (Schalter + Einstellungen).
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <strings.h>
#include <stdarg.h>
#include "web_ui.h"
#include "app_config.h"
#include "ir_light.h"
#include "mqtt_ctrl.h"
#include "rtsp_server.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "web";
static char s_ip[16] = "-";

void web_ui_set_ip(const char *ip)
{
    strlcpy(s_ip, ip, sizeof(s_ip));
}

/* ------------------------------------------------------------------------- */

static void html_escape(const char *in, char *out, size_t len)
{
    size_t o = 0;
    for (; *in && o + 7 < len; in++) {
        switch (*in) {
        case '<': o += snprintf(out + o, len - o, "&lt;"); break;
        case '>': o += snprintf(out + o, len - o, "&gt;"); break;
        case '&': o += snprintf(out + o, len - o, "&amp;"); break;
        case '"': o += snprintf(out + o, len - o, "&quot;"); break;
        default: out[o++] = *in; break;
        }
    }
    out[o] = 0;
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

static bool form_get(const char *body, const char *key, char *out, size_t len)
{
    if (httpd_query_key_value(body, key, out, len) != ESP_OK) {
        out[0] = 0;
        return false;
    }
    url_decode(out);
    return true;
}

static char *read_body(httpd_req_t *req, size_t max)
{
    if (req->content_len == 0 || req->content_len > max) {
        return NULL;
    }
    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        return NULL;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[got] = 0;
    return buf;
}

/* ------------------------------------------------------------------------- */

static const char PAGE_HEAD[] =
    "<!DOCTYPE html><html lang=de><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>P4-ETH-RTSP-Cam-MK1</title><style>"
    "body{background:#111418;color:#e6e6e6;font-family:system-ui,Segoe UI,sans-serif;margin:0;padding:20px}"
    "h1{font-size:20px;margin:0 0 16px}h2{font-size:15px;color:#4fc3f7;margin:0 0 12px}"
    ".card{background:#1b2027;border:1px solid #2a313b;border-radius:10px;padding:16px;margin-bottom:16px;max-width:720px}"
    "label{display:block;font-size:12px;color:#9aa4b1;margin:8px 0 3px}"
    "input,textarea,select{width:100%;box-sizing:border-box;background:#0d1014;color:#e6e6e6;border:1px solid #2f3742;"
    "border-radius:6px;padding:7px;font-family:inherit;font-size:14px}"
    "textarea{font-family:Consolas,monospace;height:90px}"
    "button{background:#1976d2;color:#fff;border:0;border-radius:6px;padding:8px 16px;margin-top:12px;cursor:pointer;font-size:14px}"
    "button.sec{background:#2f3742}"
    ".row{display:flex;gap:12px}.row>div{flex:1}"
    "table{border-collapse:collapse}td{padding:3px 12px 3px 0;font-size:14px}td:first-child{color:#9aa4b1}"
    "code{color:#ffcc80}.ok{color:#81c784}.bad{color:#e57373}#res{margin-top:10px;font-size:13px}"
    "</style></head><body>";

static void send_fmt(httpd_req_t *req, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    httpd_resp_sendstr_chunk(req, buf);
}

static esp_err_t h_root(httpd_req_t *req)
{
    const app_config_t *c = &g_cfg;
    char e1[160], e2[80], e3[80], e4[40];

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    send_fmt(req, "<h1>P4-ETH-RTSP-Cam-MK1 <span style='color:#9aa4b1;font-size:13px'>v%s</span></h1>",
             esp_app_get_description()->version);

    /* Status */
    send_fmt(req,
             "<div class=card><h2>Status</h2><table>"
             "<tr><td>IP-Adresse</td><td>%s</td></tr>"
             "<tr><td>RTSP-Stream</td><td><code>rtsp://%s:%d/%s</code></td></tr>"
             "<tr><td>RTSP-Clients</td><td>%d</td></tr>"
             "<tr><td>MQTT</td><td class=%s>%s</td></tr>"
             "<tr><td>Schalt-Topic</td><td><code>%s</code> = 0 / 1</td></tr>"
             "<tr><td>IR-GPIO</td><td>%s%d%s</td></tr>"
             "<tr><td>Laufzeit</td><td>%lld min</td></tr>"
             "</table></div>",
             s_ip, s_ip, CONFIG_APP_RTSP_PORT, CONFIG_APP_RTSP_PATH,
             rtsp_server_active_clients(),
             c->mqtt_uri[0] == 0 ? "bad" : (mqtt_ctrl_connected() ? "ok" : "bad"),
             c->mqtt_uri[0] == 0 ? "nicht konfiguriert" : (mqtt_ctrl_connected() ? "verbunden" : "nicht verbunden"),
             c->mqtt_topic, c->ir_gpio < 0 ? "aus " : "", c->ir_gpio, c->ir_active_low ? " (Low-aktiv)" : "",
             esp_timer_get_time() / 60000000LL);

    /* IR-Beleuchtung */
    send_fmt(req,
        "<div class=card><h2>IR-Beleuchtung</h2>"
        "<div style='display:flex;align-items:center;gap:16px'>"
        "<span id=st class=%s style='font-size:22px;font-weight:600'>%s</span>"
        "<button onclick=sw(1)>Ein</button><button class=sec onclick=sw(0)>Aus</button></div>%s</div>"
        "<script>function sw(v){fetch('/licht',{method:'POST',body:String(v)}).then(x=>x.json())"
        ".then(j=>{var e=document.getElementById('st');e.textContent=j.on?'EIN':'AUS';e.className=j.on?'ok':'bad'})}</script>",
        ir_light_get() ? "ok" : "bad", ir_light_get() ? "EIN" : "AUS",
        ir_light_available() ? "" : "<p class=bad>Kein GPIO konfiguriert - Ausgang inaktiv.</p>");

    /* Konfiguration */
    httpd_resp_sendstr_chunk(req, "<form class=card method=post action=/save><h2>Einstellungen</h2>");
    html_escape(c->hostname, e4, sizeof(e4));
    send_fmt(req, "<label>Hostname</label><input name=hostname value=\"%s\" maxlength=31>", e4);
    send_fmt(req,
             "<label><input type=checkbox name=static_ip value=1 style='width:auto' %s> Statische IP-Adresse (sonst DHCP)</label>"
             "<div class=row><div><label>IP</label><input name=ip value=\"%s\"></div>"
             "<div><label>Netzmaske</label><input name=mask value=\"%s\"></div>"
             "<div><label>Gateway</label><input name=gw value=\"%s\"></div></div>",
             c->static_ip ? "checked" : "", c->ip, c->netmask, c->gateway);
    html_escape(c->mqtt_uri, e1, sizeof(e1));
    html_escape(c->mqtt_user, e2, sizeof(e2));
    html_escape(c->mqtt_topic, e3, sizeof(e3));
    send_fmt(req,
             "<label>MQTT-Broker (leer = aus), z. B. <code>mqtt://192.168.1.10:1883</code></label>"
             "<input name=mqtt_uri value=\"%s\" maxlength=127>"
             "<div class=row><div><label>Benutzer</label><input name=mqtt_user value=\"%s\" maxlength=63></div>"
             "<div><label>Passwort (leer = unveraendert)</label><input type=password name=mqtt_pass maxlength=63></div></div>"
             "<label><input type=checkbox name=mqtt_pass_clear value=1 style='width:auto'> Passwort loeschen</label>"
             "<label>Schalt-Topic der IR-Beleuchtung (Nutzlast 0 = aus, 1 = ein)</label>"
             "<input name=mqtt_topic value=\"%s\" maxlength=63>"
             "<label>GPIO der IR-Beleuchtung (-1 = aus)</label><input name=ir_gpio type=number min=-1 max=54 value=%d>"
             "<label><input type=checkbox name=ir_active_low value=1 style='width:auto' %s> Ausgang Low-aktiv (Low = ein)</label>",
             e1, e2, e3, c->ir_gpio, c->ir_active_low ? "checked" : "");
#if CONFIG_IDF_TARGET_ESP32S3
    static const int widths[] = { 640, 800, 1024, 1280, 1280 + 1, 1600, 1920 };
    static const char *names[] = { "640 x 480 (VGA)", "800 x 600 (SVGA)", "1024 x 768 (XGA)", "1280 x 720 (HD)",
                                   "1280 x 1024 (SXGA)", "1600 x 1200 (UXGA)", "1920 x 1080 (Full HD)" };
    httpd_resp_sendstr_chunk(req, "<label>Kamera-Aufloesung (MJPEG)</label><select name=cam_width>");
    for (int i = 0; i < 7; i++) {
        send_fmt(req, "<option value=%d %s>%s</option>", widths[i],
                 widths[i] == c->cam_width ? "selected" : "", names[i]);
    }
    send_fmt(req, "</select><label>JPEG-Qualitaet (4 = beste, 40 = kleinste Datenmenge)</label>"
                  "<input name=cam_quality type=number min=4 max=40 value=%d>", c->cam_quality);
#endif
    httpd_resp_sendstr_chunk(req, "<button type=submit>Speichern &amp; Neustart</button></form>");

    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t h_save(httpd_req_t *req)
{
    char *body = read_body(req, 2048);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Ungueltige Anfrage");
        return ESP_FAIL;
    }
    app_config_t c = g_cfg;
    char v[160];

    if (form_get(body, "hostname", v, sizeof(v)) && v[0]) {
        strlcpy(c.hostname, v, sizeof(c.hostname));
    }
    c.static_ip = form_get(body, "static_ip", v, sizeof(v));
    if (form_get(body, "ip", v, sizeof(v))) {
        strlcpy(c.ip, v, sizeof(c.ip));
    }
    if (form_get(body, "mask", v, sizeof(v))) {
        strlcpy(c.netmask, v, sizeof(c.netmask));
    }
    if (form_get(body, "gw", v, sizeof(v))) {
        strlcpy(c.gateway, v, sizeof(c.gateway));
    }
    form_get(body, "mqtt_uri", v, sizeof(v));
    strlcpy(c.mqtt_uri, v, sizeof(c.mqtt_uri));
    form_get(body, "mqtt_user", v, sizeof(v));
    strlcpy(c.mqtt_user, v, sizeof(c.mqtt_user));
    if (form_get(body, "mqtt_pass", v, sizeof(v)) && v[0]) {
        strlcpy(c.mqtt_pass, v, sizeof(c.mqtt_pass));
    }
    if (form_get(body, "mqtt_pass_clear", v, sizeof(v))) {
        c.mqtt_pass[0] = 0;
    }
    if (form_get(body, "mqtt_topic", v, sizeof(v)) && v[0]) {
        size_t l = strlen(v);
        while (l > 0 && v[l - 1] == '/') {
            v[--l] = 0;
        }
        strlcpy(c.mqtt_topic, v, sizeof(c.mqtt_topic));
    }
    if (form_get(body, "ir_gpio", v, sizeof(v)) && v[0]) {
        int g = atoi(v);
        c.ir_gpio = (g < -1 || g > 54) ? -1 : g;
    }
    c.ir_active_low = form_get(body, "ir_active_low", v, sizeof(v));
#if CONFIG_IDF_TARGET_ESP32S3
    if (form_get(body, "cam_width", v, sizeof(v)) && v[0]) {
        c.cam_width = atoi(v);
    }
    if (form_get(body, "cam_quality", v, sizeof(v)) && v[0]) {
        int q = atoi(v);
        c.cam_quality = q < 4 ? 4 : (q > 40 ? 40 : q);
    }
#endif
    free(body);

    esp_err_t err = config_save(&c);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    if (err == ESP_OK) {
        httpd_resp_sendstr_chunk(req,
            "<div class=card><h2>Gespeichert</h2><p>Das Board startet neu. "
            "Bei geaenderter IP-Adresse die Seite unter der neuen Adresse aufrufen.</p>"
            "<script>setTimeout(()=>location.href='/',8000)</script></div></body></html>");
        xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    } else {
        send_fmt(req, "<div class=card><h2 class=bad>Fehler beim Speichern: %s</h2></div></body></html>",
                 esp_err_to_name(err));
    }
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t h_licht(httpd_req_t *req)
{
    char *body = read_body(req, 64);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Erwartet 0, 1 oder toggle");
        return ESP_FAIL;
    }
    bool on = ir_light_get();
    bool ok = true;
    if (body[0] == '1' || !strcasecmp(body, "on") || !strcasecmp(body, "ein")) {
        on = true;
    } else if (body[0] == '0' || !strcasecmp(body, "off") || !strcasecmp(body, "aus")) {
        on = false;
    } else if (!strcasecmp(body, "toggle")) {
        on = !on;
    } else {
        ok = false;
    }
    free(body);
    if (ok) {
        mqtt_ctrl_set_light(on);
    }
    char out[48];
    snprintf(out, sizeof(out), "{\"ok\":%s,\"on\":%s}", ok ? "true" : "false", ir_light_get() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

static esp_err_t h_status(httpd_req_t *req)
{
    char out[320];
    snprintf(out, sizeof(out),
             "{\"version\":\"%s\",\"ip\":\"%s\",\"rtsp\":\"rtsp://%s:%d/%s\",\"rtsp_clients\":%d,"
             "\"mqtt\":%s,\"ir_gpio\":%d,\"ir_licht\":%d}",
             esp_app_get_description()->version, s_ip, s_ip, CONFIG_APP_RTSP_PORT, CONFIG_APP_RTSP_PATH,
             rtsp_server_active_clients(), mqtt_ctrl_connected() ? "true" : "false",
             g_cfg.ir_gpio, ir_light_get() ? 1 : 0);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

esp_err_t web_ui_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    httpd_handle_t srv;
    esp_err_t err = httpd_start(&srv, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Webserver-Start fehlgeschlagen: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = h_root },
        { .uri = "/save", .method = HTTP_POST, .handler = h_save },
        { .uri = "/licht", .method = HTTP_POST, .handler = h_licht },
        { .uri = "/api/status", .method = HTTP_GET, .handler = h_status },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(srv, &uris[i]);
    }
    ESP_LOGI(TAG, "Webseite auf Port 80 gestartet");
    return ESP_OK;
}
