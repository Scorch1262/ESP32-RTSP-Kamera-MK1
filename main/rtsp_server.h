#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    uint16_t port;
    const char *path;     /* ohne fuehrenden '/' */
    int max_clients;
    int fps;
    uint32_t width;
    uint32_t height;
} rtsp_server_config_t;

/** Startet den RTSP-Server (eigene Task). */
esp_err_t rtsp_server_start(const rtsp_server_config_t *cfg);

/**
 * Uebergibt einen kodierten H.264-Frame (Annex-B, mit Startcodes).
 * Wird an alle Clients im Zustand PLAY gesendet.
 * @param ts90k RTP-Zeitstempel (90-kHz-Takt)
 */
void rtsp_server_push_frame(const uint8_t *data, size_t len, uint32_t ts90k, bool is_idr);

/** true, wenn ein neuer Client einen Keyframe braucht (Flag wird dabei zurueckgesetzt). */
bool rtsp_server_take_idr_request(void);

/** Anzahl Clients, die gerade Video empfangen. */
int rtsp_server_active_clients(void);
