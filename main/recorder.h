#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "rtsp_server.h"

/** Startet den Rekorder (Schreib-Task). Ohne SD-Karte bleibt er inaktiv. */
esp_err_t recorder_init(uint32_t width, uint32_t height, rtsp_codec_t codec);

/** Jedes kodierte Bild hier abgeben (JPEG bzw. H.264-Access-Unit). */
void recorder_on_frame(const uint8_t *data, size_t len, bool keyframe);

/** Meldet erkannte Bewegung (startet einen Clip, falls aktiviert). */
void recorder_motion(void);

/** Startet sofort einen Clip (z. B. Knopf auf der Webseite). */
esp_err_t recorder_start_manual(void);

/** true, wenn der H.264-Encoder fuer einen Clipbeginn ein IDR-Bild liefern soll. */
bool recorder_take_idr_request(void);

bool recorder_active(void);

/** Name der laufenden bzw. letzten Aufnahme ("" = keine). */
const char *recorder_last_file(void);

/** Anzahl Aufnahmen seit dem Start. */
unsigned recorder_count(void);
