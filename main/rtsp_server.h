#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef enum {
    RTSP_CODEC_H264 = 0,   /* RFC 6184, PT 96 */
    RTSP_CODEC_MJPEG,      /* RFC 2435, PT 26 */
} rtsp_codec_t;

typedef struct {
    uint16_t port;
    const char *path;     /* ohne fuehrenden '/' */
    int max_clients;
    int fps;
    uint32_t width;
    uint32_t height;
    rtsp_codec_t codec;
    const char *info;     /* Beschreibung fuer das SDP (i=) */
} rtsp_server_config_t;

/** Startet den RTSP-Server (eigene Task). */
esp_err_t rtsp_server_start(const rtsp_server_config_t *cfg);

/**
 * Uebergibt einen kodierten H.264-Frame (Annex-B, mit Startcodes).
 * Wird an alle Clients im Zustand PLAY gesendet.
 * @param ts90k RTP-Zeitstempel (90-kHz-Takt)
 */
void rtsp_server_push_frame(const uint8_t *data, size_t len, uint32_t ts90k, bool is_idr);

/**
 * Uebergibt ein komplettes JPEG-Bild (SOI..EOI, Baseline, 4:2:2 oder 4:2:0).
 * Wird nach RFC 2435 paketiert.
 */
void rtsp_server_push_jpeg(const uint8_t *data, size_t len, uint32_t ts90k);

/** true, wenn ein neuer Client einen Keyframe braucht (Flag wird dabei zurueckgesetzt). */
bool rtsp_server_take_idr_request(void);

/**
 * Stream ein-/ausschalten. Ausgeschaltet werden alle verbundenen Clients getrennt und
 * neue Verbindungen mit "503 Service Unavailable" abgelehnt. Kamera, Aufnahmen und
 * Bewegungserkennung laufen weiter.
 */
void rtsp_server_set_enabled(bool on);

/** true, wenn der Stream eingeschaltet ist. */
bool rtsp_server_enabled(void);

/** Anzahl Clients, die gerade Video empfangen. */
int rtsp_server_active_clients(void);
