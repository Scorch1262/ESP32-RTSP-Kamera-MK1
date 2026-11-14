/*
 * Konfigurations-Webseite: Netzwerk, WLAN, MQTT, IR-Beleuchtung, Bewegungsaufnahmen (SD-Karte).
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
#include "wifi_ctrl.h"
#include "sdcard.h"
#include "recorder.h"
#include "motion.h"

int app_dark_state(void);   /* main.c */
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
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

/* Formatierte Ausgabe ohne feste Laengengrenze (Puffer passend allokiert) */
static void send_fmt(httpd_req_t *req, const char *fmt, ...)
{
    char small[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof(small), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n < (int)sizeof(small)) {
        httpd_resp_sendstr_chunk(req, small);
        return;
    }
    char *buf = malloc(n + 1);
    if (!buf) {
        ESP_LOGE(TAG, "Kein Speicher fuer Seitenausgabe (%d Byte)", n);
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf, n + 1, fmt, ap);
    va_end(ap);
    httpd_resp_sendstr_chunk(req, buf);
    free(buf);
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
             "<tr><td>MQTT-Topics</td><td><code>%s/IR-Licht</code> = 0 / 1 (Zustand und Steuerung im selben Topic)</td></tr>"
             "<tr><td>IR-GPIO</td><td>%s%d%s</td></tr>"
             "<tr><td>Laufzeit</td><td>%lld min</td></tr>",
             s_ip, s_ip, CONFIG_APP_RTSP_PORT, CONFIG_APP_RTSP_PATH,
             rtsp_server_active_clients(),
             c->mqtt_uri[0] == 0 ? "bad" : (mqtt_ctrl_connected() ? "ok" : "bad"),
             c->mqtt_uri[0] == 0 ? "nicht konfiguriert" : (mqtt_ctrl_connected() ? "verbunden" : "nicht verbunden"),
             c->mqtt_topic, c->ir_gpio < 0 ? "aus " : "", c->ir_gpio, c->ir_active_low ? " (Low-aktiv)" : "",
             esp_timer_get_time() / 60000000LL);
    {
        char sdi[64];
        sdcard_info(sdi, sizeof(sdi));
        const char *wip = wifi_ctrl_ip();
        int64_t lm = motion_last_time();
        char lms[48] = "noch keine";
        if (lm) {
            snprintf(lms, sizeof(lms), "vor %lld s (Pegel %d %%)", (esp_timer_get_time() - lm) / 1000000LL,
                     motion_last_level() / 10);
        }
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char bright[64] = "noch keine Messung";
        int bp = motion_brightness_pct();
        int dk = app_dark_state();
        if (bp >= 0) {
            snprintf(bright, sizeof(bright), "%d %% &ndash; Dunkel: <b>%d</b>", bp, dk > 0 ? 1 : 0);
        }
        char ts[32];
        if (tm.tm_year + 1900 >= 2024) {
            strftime(ts, sizeof(ts), "%d.%m.%Y %H:%M:%S", &tm);
        } else {
            strlcpy(ts, "unbekannt (kein NTP)", sizeof(ts));
        }
        send_fmt(req,
                 "<tr><td>WLAN</td><td>%s%s%s</td></tr>"
                 "<tr><td>Uhrzeit</td><td>%s</td></tr>"
                 "<tr><td>MicroSD</td><td class=%s>%s</td></tr>"
                 "<tr><td>Letzte Bewegung</td><td>%s</td></tr>"
                 "<tr><td>Helligkeit</td><td>%s</td></tr>"
                 "<tr><td>Aufzeichnung</td><td class=%s><b>%d</b> &ndash; %s</td></tr>"
                 "<tr><td>Letzte Datei</td><td>%s</td></tr>"
                 "</table></div>",
                 wifi_ctrl_state(), wip ? " &ndash; Web: http://" : "", wip ? wip : "",
                 ts,
                 sdcard_ok() ? "ok" : "bad", sdi,
                 lms, bright,
                 recorder_active() ? "ok" : "", recorder_active() ? 1 : 0,
                 recorder_active() ? recorder_reason_text() : (recorder_ready() ? "bereit" : "nicht moeglich (keine SD-Karte)"),
                 recorder_last_file()[0] ? recorder_last_file() : "-");
        if (tm.tm_year + 1900 < 2024) {
            /* Uhrzeit aus dem Browser uebernehmen, wenn kein NTP-Server erreichbar ist */
            httpd_resp_sendstr_chunk(req,
                "<script>fetch('/api/settime',{method:'POST',body:String(Math.floor(Date.now()/1000))+','+"
                "String(-new Date().getTimezoneOffset())})</script>");
        }
    }

    /* Aufnahmen */
    send_fmt(req,
        "<div class=card><h2>Aufnahmen (MicroSD)</h2>"
        "<p style='font-size:13px;color:#9aa4b1'>Bei Bewegung im Bild wird ein Clip von %d s gespeichert (%s).</p>"
        "<p style='font-size:13px;color:#9aa4b1'>Daueraufnahme: <b class=%s>%s</b> &ndash; Segmente zu %d s, "
        "bei voller Karte werden die aeltesten Aufnahmen ueberschrieben (Ringspeicher).</p>"
        "<a href=/aufnahmen><button>Aufnahmen anzeigen / herunterladen</button></a> "
        "<button class=sec onclick=\"fetch('/rec/start',{method:'POST'}).then(x=>x.text()).then(t=>alert(t))\">"
        "Clip jetzt aufnehmen</button> "
        "<button style='background:%s' onclick=\"fetch('/rec/dauer',{method:'POST',body:'%d'})"
        ".then(x=>x.text()).then(t=>{alert(t);location.reload()})\">%s</button></div>",
        c->rec_seconds, c->rec_enable ? "Bewegungsaufnahme an" : "Bewegungsaufnahme aus",
        recorder_continuous() ? "ok" : "", recorder_continuous() ? "EIN" : "AUS", c->rec_segment_s,
        recorder_continuous() ? "#c62828" : "#2e7d32", recorder_continuous() ? 0 : 1,
        recorder_continuous() ? "Daueraufnahme beenden" : "Daueraufnahme starten");

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
             "<label>MQTT-Basis-Topic (z. B. Tuer/Kamera &rarr; Tuer/Kamera/IR-Licht, Tuer/Kamera/Aufzeichnung ...)</label>"
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
    /* Bilddrehung */
    {
        int r = c->video_rotation;
        send_fmt(req,
            "<h2 style='margin-top:18px'>Videobild</h2>"
            "<label>Bild drehen (im Uhrzeigersinn) &ndash; gilt fuer Stream, Aufnahmen und Bewegungserkennung</label>"
            "<select name=video_rotation>"
            "<option value=0 %s>0&deg; (nicht gedreht)</option>"
            "<option value=90 %s>90&deg; rechts</option>"
            "<option value=180 %s>180&deg; (auf dem Kopf)</option>"
            "<option value=270 %s>270&deg; rechts (= 90&deg; links)</option></select>"
#if CONFIG_IDF_TARGET_ESP32S3
            "<p style='font-size:12px;color:#9aa4b1'>ESP32-S3: 180&deg; dreht der Kamerasensor ohne Verlust. "
            "Bei 90&deg;/270&deg; wird jedes Bild per Software neu kodiert &ndash; dann max. 640&times;480 "
            "(Hochformat 480&times;640) und weniger Bilder pro Sekunde.</p>"
#else
            "<p style='font-size:12px;color:#9aa4b1'>ESP32-P4: Drehung verlustfrei per Grafikbaustein (PPA); "
            "bei 90&deg;/270&deg; liefert der Stream 720&times;1280 (Hochformat).</p>"
#endif
            ,
            r == 0 ? "selected" : "", r == 90 ? "selected" : "", r == 180 ? "selected" : "", r == 270 ? "selected" : "");
    }

    /* WLAN */
    if (wifi_ctrl_available()) {
        char es[80];
        html_escape(c->wifi_ssid, es, sizeof(es));
        send_fmt(req,
            "<h2 style='margin-top:18px'>WLAN</h2>"
            "<label>Betriebsart</label><select name=wifi_mode>"
            "<option value=0 %s>Aus</option>"
            "<option value=1 %s>Mit vorhandenem WLAN verbinden</option>"
            "<option value=2 %s>Eigener Access Point (IP 192.168.4.1)</option></select>"
            "<label>WLAN-Name (SSID) &ndash; beim Access Point leer = automatisch</label>"
            "<div class=row><div style='flex:3'><input name=wifi_ssid id=ssid list=ssids value=\"%s\" maxlength=32>"
            "<datalist id=ssids></datalist></div>"
            "<div style='flex:1'><button type=button class=sec style='margin-top:0;width:100%%' onclick=scan()>WLAN suchen</button></div></div>"
            "<label>WLAN-Passwort (leer = unveraendert; Access Point: mind. 8 Zeichen, sonst offen)</label>"
            "<input type=password name=wifi_pass maxlength=64>"
            "<label><input type=checkbox name=wifi_pass_clear value=1 style='width:auto'> WLAN-Passwort loeschen</label>"
            "<script>function scan(){var d=document.getElementById('ssids');d.innerHTML='';"
            "fetch('/api/wifi-scan').then(x=>x.json()).then(l=>{l.forEach(a=>{var o=document.createElement('option');"
            "o.value=a.ssid;o.label=a.rssi+' dBm'+(a.open?' (offen)':'');d.appendChild(o)});"
            "alert(l.length+' WLAN(s) gefunden - im Feld SSID auswaehlen')})}</script>",
            c->wifi_mode == 0 ? "selected" : "", c->wifi_mode == 1 ? "selected" : "",
            c->wifi_mode == 2 ? "selected" : "", es);
    } else {
        httpd_resp_sendstr_chunk(req, "<h2 style='margin-top:18px'>WLAN</h2>"
            "<p style='font-size:13px;color:#9aa4b1'>Dieses Board hat kein WLAN.</p>");
    }

    /* Aufnahme */
    {
        char en[80];
        html_escape(c->ntp_server, en, sizeof(en));
        send_fmt(req,
            "<h2 style='margin-top:18px'>Bewegungsaufnahme (MicroSD)</h2>"
            "<label><input type=checkbox name=rec_enable value=1 style='width:auto' %s> Bei Bewegung aufnehmen</label>"
            "<div class=row><div><label>Clip-Laenge (Sekunden, 5&ndash;600)</label>"
            "<input name=rec_seconds type=number min=5 max=600 value=%d></div>"
            "<div><label>Empfindlichkeit (1 = wenig, 100 = sehr empfindlich)</label>"
            "<input name=motion_sens type=number min=1 max=100 value=%d></div></div>"
            "<label><input type=checkbox name=rec_autodelete value=1 style='width:auto' %s> "
            "Aelteste Aufnahmen automatisch loeschen, wenn die Karte voll ist</label>"
            "<label>Segmentlaenge der Daueraufnahme (Sekunden, 60&ndash;3600)</label>"
            "<input name=rec_segment_s type=number min=60 max=3600 value=%d>"
            "<label>NTP-Zeitserver (fuer Dateinamen mit Datum/Uhrzeit)</label>"
            "<input name=ntp_server value=\"%s\" maxlength=47>"
            "<h2 style='margin-top:18px'>Dunkel-Meldung (MQTT &lt;Basis-Topic&gt;/Dunkel)</h2>"
            "<div class=row><div><label>Dunkel unter Helligkeit (%%)</label>"
            "<input name=dark_pct type=number min=1 max=90 value=%d></div>"
            "<div><label>Bei eingeschaltetem IR-Licht erst ab (%%) wieder hell</label>"
            "<input name=dark_ir_pct type=number min=1 max=100 value=%d></div></div>",
            c->rec_enable ? "checked" : "", c->rec_seconds, c->motion_sens,
            c->rec_autodelete ? "checked" : "", c->rec_segment_s, en, c->dark_pct, c->dark_ir_pct);
    }
    httpd_resp_sendstr_chunk(req, "<button type=submit>Speichern &amp; Neustart</button></form>");

    httpd_resp_sendstr_chunk(req, "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    recorder_finish(4000);    /* laufende Aufnahme sauber abschliessen, sonst ist die Datei unbrauchbar */
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
    if (wifi_ctrl_available()) {
        if (form_get(body, "wifi_mode", v, sizeof(v)) && v[0]) {
            int m = atoi(v);
            c.wifi_mode = (m < 0 || m > 2) ? 0 : m;
        }
        if (form_get(body, "wifi_ssid", v, sizeof(v))) {
            strlcpy(c.wifi_ssid, v, sizeof(c.wifi_ssid));
        }
        if (form_get(body, "wifi_pass", v, sizeof(v)) && v[0]) {
            strlcpy(c.wifi_pass, v, sizeof(c.wifi_pass));
        }
        if (form_get(body, "wifi_pass_clear", v, sizeof(v))) {
            c.wifi_pass[0] = 0;
        }
    }
    if (form_get(body, "video_rotation", v, sizeof(v)) && v[0]) {
        int r = atoi(v);
        c.video_rotation = (r == 90 || r == 180 || r == 270) ? r : 0;
    }
    c.rec_enable = form_get(body, "rec_enable", v, sizeof(v));
    c.rec_autodelete = form_get(body, "rec_autodelete", v, sizeof(v));
    if (form_get(body, "rec_seconds", v, sizeof(v)) && v[0]) {
        int n = atoi(v);
        c.rec_seconds = n < 5 ? 5 : (n > 600 ? 600 : n);
    }
    if (form_get(body, "motion_sens", v, sizeof(v)) && v[0]) {
        int n = atoi(v);
        c.motion_sens = n < 1 ? 1 : (n > 100 ? 100 : n);
    }
    if (form_get(body, "rec_segment_s", v, sizeof(v)) && v[0]) {
        int n = atoi(v);
        c.rec_segment_s = n < 60 ? 60 : (n > 3600 ? 3600 : n);
    }
    if (form_get(body, "dark_pct", v, sizeof(v)) && v[0]) {
        int n = atoi(v);
        c.dark_pct = n < 1 ? 1 : (n > 90 ? 90 : n);
    }
    if (form_get(body, "dark_ir_pct", v, sizeof(v)) && v[0]) {
        int n = atoi(v);
        c.dark_ir_pct = n < 1 ? 1 : (n > 100 ? 100 : n);
    }
    if (form_get(body, "ntp_server", v, sizeof(v))) {
        strlcpy(c.ntp_server, v, sizeof(c.ntp_server));
    }
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
    char out[512];
    snprintf(out, sizeof(out),
             "{\"version\":\"%s\",\"ip\":\"%s\",\"rtsp\":\"rtsp://%s:%d/%s\",\"rtsp_clients\":%d,"
             "\"mqtt\":%s,\"ir_gpio\":%d,\"ir_licht\":%d,\"aufzeichnung\":%d,\"daueraufnahme\":%d,"
             "\"bewegung\":%d,\"helligkeit\":%d,\"dunkel\":%d}",
             esp_app_get_description()->version, s_ip, s_ip, CONFIG_APP_RTSP_PORT, CONFIG_APP_RTSP_PATH,
             rtsp_server_active_clients(), mqtt_ctrl_connected() ? "true" : "false",
             g_cfg.ir_gpio, ir_light_get() ? 1 : 0, recorder_active() ? 1 : 0, recorder_continuous() ? 1 : 0,
             motion_active() ? 1 : 0, motion_brightness_pct(), app_dark_state() > 0 ? 1 : 0);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

/* ------------------------------------------------------------------------- */
/* Aufnahmen                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct {
    char name[64];
    uint32_t size;
} rec_file_t;

static int cmp_desc(const void *a, const void *b)
{
    return strcmp(((const rec_file_t *)b)->name, ((const rec_file_t *)a)->name);
}

/* Nur einfache Dateinamen zulassen (kein Pfad) */
static bool name_ok(const char *n)
{
    if (!n[0] || strlen(n) > 63 || strstr(n, "..")) {
        return false;
    }
    for (const char *p = n; *p; p++) {
        if (*p == '/' || *p == '\\') {
            return false;
        }
    }
    return true;
}

static esp_err_t h_aufnahmen(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);
    httpd_resp_sendstr_chunk(req, "<h1>Aufnahmen <a href=/ style='font-size:13px;color:#4fc3f7'>&larr; zurueck</a></h1>");
    if (!sdcard_ok()) {
        httpd_resp_sendstr_chunk(req, "<div class=card><p class=bad>Keine MicroSD-Karte gefunden "
                                      "(FAT32 formatiert einlegen und Board neu starten).</p></div></body></html>");
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_OK;
    }
    char sdi[64];
    sdcard_info(sdi, sizeof(sdi));

    int cap = 256, n = 0;
    rec_file_t *files = malloc(cap * sizeof(rec_file_t));
    DIR *d = opendir(SD_REC_DIR);
    if (d && files) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_type == DT_DIR) {
                continue;
            }
            if (n == cap) {
                rec_file_t *nf = realloc(files, cap * 2 * sizeof(rec_file_t));
                if (!nf) {
                    break;
                }
                files = nf;
                cap *= 2;
            }
            strlcpy(files[n].name, e->d_name, sizeof(files[n].name));
            char path[300];
            struct stat st;
            snprintf(path, sizeof(path), SD_REC_DIR "/%s", e->d_name);
            files[n].size = (stat(path, &st) == 0) ? (uint32_t)st.st_size : 0;
            n++;
        }
        closedir(d);
    }
    if (files && n > 1) {
        qsort(files, n, sizeof(rec_file_t), cmp_desc);
    }
    send_fmt(req, "<div class=card style='max-width:900px'><h2>%d Datei(en) &ndash; Karte: %s</h2>"
                  "<table style='width:100%%'><tr><td>Datei</td><td>Groesse</td><td></td></tr>", n, sdi);
    for (int i = 0; i < n; i++) {
        bool cur = recorder_active() && strcmp(files[i].name, recorder_last_file()) == 0;
        send_fmt(req,
                 "<tr><td>%s%s</td><td>%.1f MB</td><td style='white-space:nowrap'>"
                 "<a href=\"/sd/%s\" download><button style='margin:2px'>Herunterladen</button></a>"
                 "<button class=sec style='margin:2px' onclick=\"del('%s')\">Loeschen</button></td></tr>",
                 files[i].name, cur ? " <span class=ok>(laeuft)</span>" : "", files[i].size / 1048576.0,
                 files[i].name, files[i].name);
    }
    httpd_resp_sendstr_chunk(req,
        "</table></div>"
        "<div class=card style='max-width:900px;border-color:#5c2b2b'><h2 style='color:#e57373'>MicroSD-Karte leeren</h2>"
        "<p style='font-size:13px;color:#9aa4b1'>Loescht <b>alle</b> Dateien und Ordner auf der Karte &ndash; "
        "auch Dateien, die nicht von dieser Kamera stammen. Eine laufende Aufnahme wird vorher beendet, "
        "eine eingeschaltete Daueraufnahme laeuft danach weiter.</p>"
        "<button style='background:#c62828' onclick=wipe()>Alles von der SD-Karte loeschen</button></div>"
        "<script>function del(n){if(confirm(n+' loeschen?'))"
        "fetch('/sd-loeschen',{method:'POST',body:n}).then(()=>location.reload())}"
        "function wipe(){if(!confirm('Wirklich ALLE Dateien auf der MicroSD-Karte loeschen?'))return;"
        "if(prompt('Zur Bestaetigung LOESCHEN eingeben:')!=='LOESCHEN')return;"
        "fetch('/sd-alles-loeschen',{method:'POST',body:'LOESCHEN'}).then(x=>x.text())"
        ".then(t=>{alert(t);location.reload()})}</script></body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    free(files);
    return ESP_OK;
}

static esp_err_t h_download(httpd_req_t *req)
{
    const char *name = req->uri + 4;          /* hinter "/sd/" */
    char dec[96];
    strlcpy(dec, name, sizeof(dec));
    char *q = strchr(dec, '?');
    if (q) {
        *q = 0;
    }
    url_decode(dec);
    if (!sdcard_ok() || !name_ok(dec)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Datei nicht gefunden");
        return ESP_FAIL;
    }
    char path[128];
    snprintf(path, sizeof(path), SD_REC_DIR "/%s", dec);
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Datei nicht gefunden");
        return ESP_FAIL;
    }
    struct stat st;
    char len_s[16], disp[128];
    if (stat(path, &st) == 0) {
        snprintf(len_s, sizeof(len_s), "%ld", (long)st.st_size);
        httpd_resp_set_hdr(req, "Content-Length", len_s);
    }
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", dec);
    httpd_resp_set_type(req, "video/x-msvideo");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    const size_t bs = 16 * 1024;
    char *buf = malloc(bs);
    esp_err_t ret = ESP_OK;
    size_t r;
    while (buf && (r = fread(buf, 1, bs, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) {
            ret = ESP_FAIL;
            break;
        }
    }
    fclose(f);
    free(buf);
    if (ret == ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    return ret;
}

static esp_err_t h_delete(httpd_req_t *req)
{
    char *body = read_body(req, 96);
    if (!body || !name_ok(body) || !sdcard_ok()) {
        free(body);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Ungueltiger Dateiname");
        return ESP_FAIL;
    }
    if (recorder_active() && strcmp(body, recorder_last_file()) == 0) {
        free(body);
        httpd_resp_sendstr(req, "Aufnahme laeuft noch");
        return ESP_OK;
    }
    char path[128];
    snprintf(path, sizeof(path), SD_REC_DIR "/%s", body);
    int r = unlink(path);
    ESP_LOGI(TAG, "Aufnahme geloescht: %s (%s)", body, r == 0 ? "ok" : "Fehler");
    free(body);
    httpd_resp_sendstr(req, r == 0 ? "geloescht" : "Fehler");
    return ESP_OK;
}

static esp_err_t h_wipe(httpd_req_t *req)
{
    char *body = read_body(req, 16);
    bool ok = body && strcmp(body, "LOESCHEN") == 0;
    free(body);
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bestaetigung fehlt");
        return ESP_FAIL;
    }
    if (!sdcard_ok()) {
        httpd_resp_sendstr(req, "Keine MicroSD-Karte");
        return ESP_OK;
    }
    recorder_finish(4000);          /* laufende Aufnahme sauber beenden */
    int files, dirs, errors;
    sdcard_wipe(&files, &dirs, &errors);
    recorder_resume();
    char out[128];
    snprintf(out, sizeof(out), "%d Datei(en) und %d Ordner geloescht%s", files, dirs,
             errors ? " - einige Eintraege konnten nicht geloescht werden" : "");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

static esp_err_t h_rec_start(httpd_req_t *req)
{
    esp_err_t err = recorder_start_manual();
    httpd_resp_sendstr(req, err == ESP_OK ? "Aufnahme gestartet" :
                       "Keine Aufnahme moeglich (MicroSD-Karte oder Kamera fehlt)");
    return ESP_OK;
}

static esp_err_t h_rec_dauer(httpd_req_t *req)
{
    char *body = read_body(req, 8);
    bool on = body && body[0] == '1';
    free(body);
    esp_err_t err = recorder_set_continuous(on);
    httpd_resp_sendstr(req, err == ESP_OK ? (on ? "Daueraufnahme gestartet (Ringspeicher)" : "Daueraufnahme beendet")
                                          : "Keine Daueraufnahme moeglich (MicroSD-Karte oder Kamera fehlt)");
    return ESP_OK;
}

static esp_err_t h_wifi_scan(httpd_req_t *req)
{
    char *out = malloc(3072);
    if (!out) {
        return httpd_resp_send_500(req);
    }
    wifi_ctrl_scan_json(out, 3072);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, out);
    free(out);
    return ESP_OK;
}

/* Uhrzeit vom Browser uebernehmen ("<unix-sekunden>,<utc-offset-minuten>"),
   nur solange keine gueltige Zeit (NTP) vorhanden ist */
static esp_err_t h_settime(httpd_req_t *req)
{
    char *body = read_body(req, 40);
    if (body) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        long long t = atoll(body);
        if (tm.tm_year + 1900 < 2024 && t > 1700000000LL) {
            struct timeval tv = { .tv_sec = (time_t)t };
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "Uhrzeit vom Browser uebernommen");
        }
        free(body);
    }
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

esp_err_t web_ui_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 20;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.send_wait_timeout = 30;
    httpd_handle_t srv;
    esp_err_t err = httpd_start(&srv, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Webserver-Start fehlgeschlagen: %s", esp_err_to_name(err));
        return err;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = h_root },
        { .uri = "/aufnahmen", .method = HTTP_GET, .handler = h_aufnahmen },
        { .uri = "/sd/*", .method = HTTP_GET, .handler = h_download },
        { .uri = "/sd-loeschen", .method = HTTP_POST, .handler = h_delete },
        { .uri = "/sd-alles-loeschen", .method = HTTP_POST, .handler = h_wipe },
        { .uri = "/rec/start", .method = HTTP_POST, .handler = h_rec_start },
        { .uri = "/rec/dauer", .method = HTTP_POST, .handler = h_rec_dauer },
        { .uri = "/api/wifi-scan", .method = HTTP_GET, .handler = h_wifi_scan },
        { .uri = "/api/settime", .method = HTTP_POST, .handler = h_settime },
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
