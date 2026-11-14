#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "rtsp_server.h"

typedef struct {
    uint32_t width;
    uint32_t height;
    int fps;
    rtsp_codec_t codec;
    const char *desc;     /* z. B. "OV5640 MIPI-CSI, H.264 (Hardware)" */
} video_info_t;

/** Initialisiert Kamera (und ggf. Encoder). */
esp_err_t video_init(video_info_t *info);

/** Startet die Task, die Bilder an den RTSP-Server liefert. */
void video_start(void);
