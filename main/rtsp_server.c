/*
 * Schlanker RTSP-Server (RFC 2326) fuer einen H.264-Videostream.
 *
 *  - Methoden: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER, SET_PARAMETER
 *  - Transport: RTP/AVP/TCP (interleaved, z. B. "rtsp_transport tcp")
 *               RTP/AVP     (UDP unicast)
 *  - Paketierung: RFC 6184, Single-NAL und FU-A (packetization-mode=1)
 */
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include "rtsp_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_app_desc.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

static const char *TAG = "rtsp";

#define RTSP_RX_BUF          2048
#define RTSP_TX_BUF          1536
#define RTP_PT_H264          96
#define RTP_MAX_PAYLOAD      CONFIG_APP_RTP_MAX_PAYLOAD
#define SESSION_TIMEOUT_S    60
#define UDP_IDLE_TIMEOUT_US  (90LL * 1000 * 1000)
#define MAX_NALS_PER_FRAME   32
#define PARAM_SET_MAX        128

typedef struct {
    bool used;
    bool playing;
    bool setup_done;
    volatile bool broken;
    bool wait_idr;
    bool tcp;
    uint8_t rtp_ch;
    int sock;
    int udp_sock;
    struct sockaddr_in udp_dst;
    uint32_t session;
    uint32_t ssrc;
    uint16_t seq;
    int64_t last_activity;
    SemaphoreHandle_t tx_lock;
    char peer[16];
} rtsp_client_t;

static rtsp_server_config_t s_cfg;
static rtsp_client_t *s_clients;
static SemaphoreHandle_t s_lock;
static volatile bool s_idr_request;

static uint8_t s_sps[PARAM_SET_MAX], s_pps[PARAM_SET_MAX];
static size_t s_sps_len, s_pps_len;

static uint8_t s_pkt[4 + 12 + 2 + RTP_MAX_PAYLOAD];

/* ------------------------------------------------------------------------- */
/* Hilfsfunktionen                                                            */
/* ------------------------------------------------------------------------- */

static int send_all(int sock, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len > 0) {
        int r = send(sock, p, len, 0);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += r;
        len -= r;
    }
    return 0;
}

/* Sucht "Name:" am Zeilenanfang (case-insensitive) und kopiert den Wert. */
static bool get_header(const char *req, const char *name, char *out, size_t out_len)
{
    size_t nlen = strlen(name);
    const char *p = strstr(req, "\r\n");
    while (p) {
        p += 2;
        if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            p += nlen + 1;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            size_t i = 0;
            while (p[i] && p[i] != '\r' && p[i] != '\n' && i + 1 < out_len) {
                out[i] = p[i];
                i++;
            }
            out[i] = 0;
            return true;
        }
        p = strstr(p, "\r\n");
    }
    return false;
}

static void local_ip(int sock, char *out, size_t len)
{
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    if (getsockname(sock, (struct sockaddr *)&addr, &alen) == 0) {
        inet_ntoa_r(addr.sin_addr, out, len);
    } else {
        strlcpy(out, "0.0.0.0", len);
    }
}

static bool url_matches(const char *url)
{
    if (s_cfg.path[0] == 0) {
        return true;
    }
    char needle[80];
    snprintf(needle, sizeof(needle), "/%s", s_cfg.path);
    const char *p = strstr(url, needle);
    if (!p) {
        return false;
    }
    char after = p[strlen(needle)];
    return after == 0 || after == '/' || after == '?';
}

static int send_reply(rtsp_client_t *c, int code, const char *reason, const char *cseq,
                      const char *extra, const char *body)
{
    char hdr[RTSP_TX_BUF];
    char sess[48] = "";
    if (c->setup_done) {
        snprintf(sess, sizeof(sess), "Session: %08" PRIX32 ";timeout=%d\r\n", c->session, SESSION_TIMEOUT_S);
    }
    size_t blen = body ? strlen(body) : 0;
    int n = snprintf(hdr, sizeof(hdr),
                     "RTSP/1.0 %d %s\r\n"
                     "CSeq: %s\r\n"
                     "Server: P4-ETH-RTSP-Cam-MK1/%s\r\n"
                     "Cache-Control: no-cache\r\n"
                     "%s%s",
                     code, reason, cseq, esp_app_get_description()->version, sess, extra ? extra : "");
    if (blen) {
        n += snprintf(hdr + n, sizeof(hdr) - n, "Content-Length: %u\r\n", (unsigned)blen);
    }
    n += snprintf(hdr + n, sizeof(hdr) - n, "\r\n");
    if (n >= (int)sizeof(hdr)) {
        ESP_LOGE(TAG, "Antwort-Header zu lang");
        return -1;
    }
    xSemaphoreTake(c->tx_lock, portMAX_DELAY);
    int r = send_all(c->sock, hdr, n);
    if (r == 0 && blen) {
        r = send_all(c->sock, body, blen);
    }
    xSemaphoreGive(c->tx_lock);
    return r;
}

/* ------------------------------------------------------------------------- */
/* RTP                                                                        */
/* ------------------------------------------------------------------------- */

static void send_rtp(rtsp_client_t *c, const uint8_t *pre, size_t pre_len,
                     const uint8_t *pl, size_t pl_len, bool marker, uint32_t ts)
{
    if (c->broken) {
        return;
    }
    uint8_t *rtp = s_pkt + 4;
    rtp[0] = 0x80;
    rtp[1] = (marker ? 0x80 : 0x00) | RTP_PT_H264;
    rtp[2] = c->seq >> 8;
    rtp[3] = c->seq & 0xFF;
    rtp[4] = ts >> 24;
    rtp[5] = ts >> 16;
    rtp[6] = ts >> 8;
    rtp[7] = ts;
    rtp[8] = c->ssrc >> 24;
    rtp[9] = c->ssrc >> 16;
    rtp[10] = c->ssrc >> 8;
    rtp[11] = c->ssrc;
    if (pre_len) {
        memcpy(rtp + 12, pre, pre_len);
    }
    memcpy(rtp + 12 + pre_len, pl, pl_len);
    size_t rtp_len = 12 + pre_len + pl_len;
    c->seq++;

    if (c->tcp) {
        s_pkt[0] = '$';
        s_pkt[1] = c->rtp_ch;
        s_pkt[2] = rtp_len >> 8;
        s_pkt[3] = rtp_len & 0xFF;
        xSemaphoreTake(c->tx_lock, portMAX_DELAY);
        int r = send_all(c->sock, s_pkt, rtp_len + 4);
        xSemaphoreGive(c->tx_lock);
        if (r < 0) {
            ESP_LOGW(TAG, "[%s] TCP-Senden fehlgeschlagen (errno %d) - Verbindung wird getrennt", c->peer, errno);
            c->broken = true;
            shutdown(c->sock, SHUT_RDWR);
        }
    } else {
        for (int attempt = 0; attempt < 3; attempt++) {
            int r = sendto(c->udp_sock, rtp, rtp_len, 0, (struct sockaddr *)&c->udp_dst, sizeof(c->udp_dst));
            if (r >= 0) {
                break;
            }
            if (errno != ENOMEM && errno != EAGAIN) {
                break;
            }
            vTaskDelay(1);   /* TX-Puffer voll: kurz warten */
        }
    }
}

static void send_nal(rtsp_client_t *c, const uint8_t *nal, size_t len, uint32_t ts, bool last_of_frame)
{
    if (len == 0) {
        return;
    }
    if (len <= RTP_MAX_PAYLOAD) {
        send_rtp(c, NULL, 0, nal, len, last_of_frame, ts);
        return;
    }
    /* FU-A */
    uint8_t hdr = nal[0];
    uint8_t fu[2];
    fu[0] = (hdr & 0xE0) | 28;
    const uint8_t *p = nal + 1;
    size_t rem = len - 1;
    bool first = true;
    int pkts = 0;
    while (rem > 0 && !c->broken) {
        size_t chunk = rem > (RTP_MAX_PAYLOAD - 2) ? (RTP_MAX_PAYLOAD - 2) : rem;
        bool end = (chunk == rem);
        fu[1] = (hdr & 0x1F) | (first ? 0x80 : 0) | (end ? 0x40 : 0);
        send_rtp(c, fu, 2, p, chunk, end && last_of_frame, ts);
        p += chunk;
        rem -= chunk;
        first = false;
        /* UDP-Bursts etwas entzerren, damit EMAC/lwIP nicht ueberlaufen */
        if (!c->tcp && (++pkts % 24) == 0) {
            vTaskDelay(1);
        }
    }
}

/* Zerlegt einen Annex-B-Datenstrom in NAL-Einheiten (ohne Startcodes). */
static const uint8_t *find_start_code(const uint8_t *p, const uint8_t *end, int *sc_len)
{
    while (p + 3 <= end) {
        if (p[0] == 0 && p[1] == 0) {
            if (p[2] == 1) {
                *sc_len = 3;
                return p;
            }
            if (p + 4 <= end && p[2] == 0 && p[3] == 1) {
                *sc_len = 4;
                return p;
            }
        }
        p++;
    }
    *sc_len = 0;
    return end;
}

void rtsp_server_push_frame(const uint8_t *data, size_t len, uint32_t ts90k, bool is_idr)
{
    const uint8_t *nal_ptr[MAX_NALS_PER_FRAME];
    size_t nal_len[MAX_NALS_PER_FRAME];
    int n = 0;

    const uint8_t *end = data + len;
    int sc;
    const uint8_t *s = find_start_code(data, end, &sc);
    while (s < end && n < MAX_NALS_PER_FRAME) {
        const uint8_t *nal = s + sc;
        int sc2;
        const uint8_t *next = find_start_code(nal, end, &sc2);
        size_t l = next - nal;
        while (l > 0 && nal[l - 1] == 0 && next != end) {  /* trailing_zero_8bits */
            l--;
        }
        if (l > 0) {
            nal_ptr[n] = nal;
            nal_len[n] = l;
            n++;
        }
        s = next;
        sc = sc2;
    }
    if (n == 0) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* SPS/PPS fuer das SDP merken */
    for (int i = 0; i < n; i++) {
        uint8_t type = nal_ptr[i][0] & 0x1F;
        if (type == 7 && nal_len[i] <= PARAM_SET_MAX) {
            memcpy(s_sps, nal_ptr[i], nal_len[i]);
            s_sps_len = nal_len[i];
        } else if (type == 8 && nal_len[i] <= PARAM_SET_MAX) {
            memcpy(s_pps, nal_ptr[i], nal_len[i]);
            s_pps_len = nal_len[i];
        }
    }

    for (int ci = 0; ci < s_cfg.max_clients; ci++) {
        rtsp_client_t *c = &s_clients[ci];
        if (!c->used || !c->playing || c->broken) {
            continue;
        }
        if (c->wait_idr) {
            if (!is_idr) {
                continue;
            }
            c->wait_idr = false;
            ESP_LOGI(TAG, "[%s] Stream startet mit IDR-Frame", c->peer);
        }
        for (int i = 0; i < n && !c->broken; i++) {
            send_nal(c, nal_ptr[i], nal_len[i], ts90k, i == n - 1);
        }
    }

    xSemaphoreGive(s_lock);
}

bool rtsp_server_take_idr_request(void)
{
    if (s_idr_request) {
        s_idr_request = false;
        return true;
    }
    return false;
}

int rtsp_server_active_clients(void)
{
    int cnt = 0;
    for (int i = 0; i < s_cfg.max_clients; i++) {
        if (s_clients[i].used && s_clients[i].playing) {
            cnt++;
        }
    }
    return cnt;
}

/* ------------------------------------------------------------------------- */
/* RTSP-Anfragen                                                              */
/* ------------------------------------------------------------------------- */

static bool wait_for_param_sets(int timeout_ms)
{
    for (int t = 0; t < timeout_ms; t += 50) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool ok = s_sps_len > 3 && s_pps_len > 0;
        xSemaphoreGive(s_lock);
        if (ok) {
            return true;
        }
        s_idr_request = true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

static void handle_describe(rtsp_client_t *c, const char *url, const char *cseq)
{
    if (!url_matches(url)) {
        send_reply(c, 404, "Not Found", cseq, NULL, NULL);
        return;
    }
    if (!wait_for_param_sets(3000)) {
        send_reply(c, 503, "Service Unavailable", cseq, NULL, NULL);
        return;
    }

    char sps_b64[PARAM_SET_MAX * 2], pps_b64[PARAM_SET_MAX * 2];
    size_t olen = 0;
    uint8_t profile[3];

    xSemaphoreTake(s_lock, portMAX_DELAY);
    mbedtls_base64_encode((unsigned char *)sps_b64, sizeof(sps_b64) - 1, &olen, s_sps, s_sps_len);
    sps_b64[olen] = 0;
    mbedtls_base64_encode((unsigned char *)pps_b64, sizeof(pps_b64) - 1, &olen, s_pps, s_pps_len);
    pps_b64[olen] = 0;
    memcpy(profile, s_sps + 1, 3);
    xSemaphoreGive(s_lock);

    char ip[16];
    local_ip(c->sock, ip, sizeof(ip));

    char sdp[1024];
    snprintf(sdp, sizeof(sdp),
             "v=0\r\n"
             "o=- %" PRIu32 " 1 IN IP4 %s\r\n"
             "s=P4-ETH-RTSP-Cam-MK1\r\n"
             "i=OV5640 H.264 Live\r\n"
             "c=IN IP4 0.0.0.0\r\n"
             "t=0 0\r\n"
             "a=range:npt=0-\r\n"
             "a=control:*\r\n"
             "m=video 0 RTP/AVP %d\r\n"
             "a=rtpmap:%d H264/90000\r\n"
             "a=fmtp:%d packetization-mode=1;profile-level-id=%02X%02X%02X;sprop-parameter-sets=%s,%s\r\n"
             "a=framesize:%d %" PRIu32 "-%" PRIu32 "\r\n"
             "a=framerate:%d\r\n"
             "a=control:track1\r\n",
             esp_random(), ip,
             RTP_PT_H264, RTP_PT_H264, RTP_PT_H264,
             profile[0], profile[1], profile[2], sps_b64, pps_b64,
             RTP_PT_H264, s_cfg.width, s_cfg.height,
             s_cfg.fps);

    char extra[256];
    snprintf(extra, sizeof(extra),
             "Content-Base: rtsp://%s:%u/%s/\r\n"
             "Content-Type: application/sdp\r\n",
             ip, s_cfg.port, s_cfg.path);
    send_reply(c, 200, "OK", cseq, extra, sdp);
}

static void handle_setup(rtsp_client_t *c, const char *req, const char *cseq)
{
    char tr[256];
    if (!get_header(req, "Transport", tr, sizeof(tr))) {
        send_reply(c, 461, "Unsupported Transport", cseq, NULL, NULL);
        return;
    }
    if (strstr(tr, "multicast")) {
        send_reply(c, 461, "Unsupported Transport", cseq, NULL, NULL);
        return;
    }

    if (!c->setup_done) {
        c->session = esp_random();
        c->ssrc = esp_random();
        c->seq = esp_random() & 0xFFFF;
    }

    char extra[256];
    if (strstr(tr, "RTP/AVP/TCP")) {
        int a = 0, b = 1;
        const char *p = strstr(tr, "interleaved=");
        if (p) {
            sscanf(p + 12, "%d-%d", &a, &b);
        }
        c->tcp = true;
        c->rtp_ch = (uint8_t)a;
        snprintf(extra, sizeof(extra),
                 "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d;ssrc=%08" PRIX32 "\r\n", a, b, c->ssrc);
    } else {
        int cp1 = 0, cp2 = 0;
        const char *p = strstr(tr, "client_port=");
        if (!p || sscanf(p + 12, "%d-%d", &cp1, &cp2) < 1 || cp1 <= 0) {
            send_reply(c, 461, "Unsupported Transport", cseq, NULL, NULL);
            return;
        }
        if (cp2 <= 0) {
            cp2 = cp1 + 1;
        }
        if (c->udp_sock >= 0) {
            close(c->udp_sock);
        }
        c->udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        struct sockaddr_in local = {
            .sin_family = AF_INET,
            .sin_port = 0,
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (c->udp_sock < 0 || bind(c->udp_sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
            send_reply(c, 500, "Internal Server Error", cseq, NULL, NULL);
            return;
        }
        socklen_t llen = sizeof(local);
        getsockname(c->udp_sock, (struct sockaddr *)&local, &llen);
        uint16_t sp = ntohs(local.sin_port);

        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        getpeername(c->sock, (struct sockaddr *)&peer, &plen);
        c->udp_dst = peer;
        c->udp_dst.sin_port = htons(cp1);
        c->tcp = false;
        snprintf(extra, sizeof(extra),
                 "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%u-%u;ssrc=%08" PRIX32 "\r\n",
                 cp1, cp2, sp, sp + 1, c->ssrc);
    }
    c->setup_done = true;
    ESP_LOGI(TAG, "[%s] SETUP %s", c->peer, c->tcp ? "RTP/TCP (interleaved)" : "RTP/UDP");
    send_reply(c, 200, "OK", cseq, extra, NULL);
}

static void handle_play(rtsp_client_t *c, const char *cseq)
{
    if (!c->setup_done) {
        send_reply(c, 455, "Method Not Valid in This State", cseq, NULL, NULL);
        return;
    }
    char extra[64];
    snprintf(extra, sizeof(extra), "Range: npt=0.000-\r\n");
    send_reply(c, 200, "OK", cseq, extra, NULL);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    c->wait_idr = true;
    c->playing = true;
    xSemaphoreGive(s_lock);
    s_idr_request = true;
    ESP_LOGI(TAG, "[%s] PLAY", c->peer);
}

/* Liefert false, wenn die Verbindung beendet werden soll. */
static bool handle_request(rtsp_client_t *c, const char *req)
{
    char method[24] = "", url[256] = "", cseq[16] = "0";
    sscanf(req, "%23s %255s", method, url);
    get_header(req, "CSeq", cseq, sizeof(cseq));
    ESP_LOGD(TAG, "[%s] %s %s", c->peer, method, url);

    if (strcmp(method, "OPTIONS") == 0) {
        send_reply(c, 200, "OK", cseq,
                   "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER, SET_PARAMETER\r\n", NULL);
    } else if (strcmp(method, "DESCRIBE") == 0) {
        handle_describe(c, url, cseq);
    } else if (strcmp(method, "SETUP") == 0) {
        handle_setup(c, req, cseq);
    } else if (strcmp(method, "PLAY") == 0) {
        handle_play(c, cseq);
    } else if (strcmp(method, "PAUSE") == 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        c->playing = false;
        xSemaphoreGive(s_lock);
        send_reply(c, 200, "OK", cseq, NULL, NULL);
    } else if (strcmp(method, "TEARDOWN") == 0) {
        send_reply(c, 200, "OK", cseq, NULL, NULL);
        return false;
    } else if (strcmp(method, "GET_PARAMETER") == 0 || strcmp(method, "SET_PARAMETER") == 0) {
        send_reply(c, 200, "OK", cseq, NULL, NULL);
    } else {
        send_reply(c, 501, "Not Implemented", cseq, NULL, NULL);
    }
    return true;
}

static void client_task(void *arg)
{
    rtsp_client_t *c = arg;
    char *buf = malloc(RTSP_RX_BUF);
    size_t used = 0;
    size_t skip = 0;   /* noch zu verwerfende Bytes eines grossen '$'-Pakets */

    ESP_LOGI(TAG, "[%s] verbunden", c->peer);

    while (buf && !c->broken) {
        int r = recv(c->sock, buf + used, RTSP_RX_BUF - 1 - used, 0);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!c->tcp && c->setup_done &&
                        esp_timer_get_time() - c->last_activity > UDP_IDLE_TIMEOUT_US) {
                    ESP_LOGW(TAG, "[%s] Session-Timeout", c->peer);
                    break;
                }
                continue;
            }
            break;
        }
        if (r == 0) {
            break;
        }
        c->last_activity = esp_timer_get_time();

        if (skip) {
            size_t d = (size_t)r < skip ? (size_t)r : skip;
            skip -= d;
            memmove(buf + used, buf + used + d, r - d);
            r -= d;
        }
        used += r;

        bool keep = true;
        while (used > 0 && keep) {
            if (buf[0] == '$') {               /* RTCP vom Client (TCP-interleaved) */
                if (used < 4) {
                    break;
                }
                size_t plen = 4 + (((uint8_t)buf[2] << 8) | (uint8_t)buf[3]);
                if (plen > used) {
                    if (plen > RTSP_RX_BUF - 1) {
                        skip = plen - used;
                        used = 0;
                    }
                    break;
                }
                memmove(buf, buf + plen, used - plen);
                used -= plen;
                continue;
            }

            buf[used] = 0;
            char *end = strstr(buf, "\r\n\r\n");
            if (!end) {
                if (used >= RTSP_RX_BUF - 1) {
                    ESP_LOGW(TAG, "[%s] Anfrage zu lang - verworfen", c->peer);
                    used = 0;
                }
                break;
            }
            size_t hdr_len = (end - buf) + 4;
            char clen_s[12];
            size_t body = 0;
            end[2] = 0;  /* Header terminieren (fuer get_header) */
            if (get_header(buf, "Content-Length", clen_s, sizeof(clen_s))) {
                body = (size_t)atoi(clen_s);
            }
            end[2] = '\r';
            if (hdr_len + body > used) {
                if (hdr_len + body > RTSP_RX_BUF - 1) {
                    used = 0;
                }
                break;
            }
            end[2] = 0;
            keep = handle_request(c, buf);
            size_t total = hdr_len + body;
            memmove(buf, buf + total, used - total);
            used -= total;
        }
        if (!keep) {
            break;
        }
    }

    ESP_LOGI(TAG, "[%s] getrennt", c->peer);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    c->playing = false;
    if (c->udp_sock >= 0) {
        close(c->udp_sock);
        c->udp_sock = -1;
    }
    close(c->sock);
    c->sock = -1;
    vSemaphoreDelete(c->tx_lock);
    c->tx_lock = NULL;
    c->used = false;
    xSemaphoreGive(s_lock);

    free(buf);
    vTaskDelete(NULL);
}

static void server_task(void *arg)
{
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls < 0) {
        ESP_LOGE(TAG, "socket() fehlgeschlagen");
        vTaskDelete(NULL);
        return;
    }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(s_cfg.port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, 4) != 0) {
        ESP_LOGE(TAG, "bind/listen auf Port %u fehlgeschlagen", s_cfg.port);
        close(ls);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "RTSP-Server lauscht auf Port %u, Pfad /%s", s_cfg.port, s_cfg.path);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int s = accept(ls, (struct sockaddr *)&peer, &plen);
        if (s < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        rtsp_client_t *c = NULL;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < s_cfg.max_clients; i++) {
            if (!s_clients[i].used) {
                c = &s_clients[i];
                memset(c, 0, sizeof(*c));
                c->used = true;
                break;
            }
        }
        xSemaphoreGive(s_lock);

        if (!c) {
            static const char busy[] = "RTSP/1.0 503 Service Unavailable\r\n\r\n";
            send(s, busy, sizeof(busy) - 1, 0);
            close(s);
            ESP_LOGW(TAG, "Maximale Clientanzahl erreicht - Verbindung abgelehnt");
            continue;
        }

        c->sock = s;
        c->udp_sock = -1;
        c->tx_lock = xSemaphoreCreateMutex();
        c->last_activity = esp_timer_get_time();
        inet_ntoa_r(peer.sin_addr, c->peer, sizeof(c->peer));

        int nodelay = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        struct timeval rto = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rto, sizeof(rto));
        struct timeval sto = { .tv_sec = 3, .tv_usec = 0 };
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &sto, sizeof(sto));
        int ka = 1, idle = 10, intvl = 5, cnt = 3;
        setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &ka, sizeof(ka));
        setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));

        if (xTaskCreate(client_task, "rtsp_cli", 6144, c, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Client-Task konnte nicht gestartet werden");
            close(s);
            vSemaphoreDelete(c->tx_lock);
            c->used = false;
        }
    }
}

esp_err_t rtsp_server_start(const rtsp_server_config_t *cfg)
{
    s_cfg = *cfg;
    s_clients = calloc(s_cfg.max_clients, sizeof(rtsp_client_t));
    s_lock = xSemaphoreCreateMutex();
    if (!s_clients || !s_lock) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(server_task, "rtsp_srv", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
