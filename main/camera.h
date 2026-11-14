#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    uint32_t width;
    uint32_t height;
    size_t   yuv_size;   /* Groesse eines YUV420-(O_UYY_E_VYY)-Bildes in Byte */
    bool     zero_copy;  /* true: CSI liefert direkt YUV420 ohne Drehung, false: PPA (Farbwandlung/Drehung) */
} camera_info_t;

typedef struct {
    uint8_t *data;       /* YUV420 (O_UYY_E_VYY), direkt vom H.264-Encoder lesbar */
    size_t   len;
    int64_t  timestamp_us;
    int      v4l2_index; /* intern */
} camera_frame_t;

/**
 * Initialisiert esp_video, oeffnet die MIPI-CSI-Kamera (OV5640) und startet den Stream.
 * @param rotation Drehung im Uhrzeigersinn: 0, 90, 180, 270 (per PPA)
 *                 info->width/height enthalten danach die gedrehte Groesse.
 */
esp_err_t camera_init(camera_info_t *info, int rotation);

/** Holt das naechste Bild (blockierend). */
esp_err_t camera_get_frame(camera_frame_t *frame);

/** Gibt ein mit camera_get_frame() geholtes Bild zurueck. */
void camera_put_frame(camera_frame_t *frame);
