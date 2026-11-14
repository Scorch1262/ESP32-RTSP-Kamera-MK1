#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "rtsp_server.h"

typedef enum {
    REC_NONE = 0,
    REC_MOTION,       /* Bewegungs-Clip */
    REC_MANUAL,       /* Knopf "Clip jetzt aufnehmen" */
    REC_CONTINUOUS,   /* Daueraufnahme (Ringspeicher) */
} rec_reason_t;

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

/** true, solange eine Aufzeichnung (gleich aus welchem Grund) laeuft. */
bool recorder_active(void);

/** Daueraufnahme als Ringspeicher ein/aus (wird gespeichert und bleibt nach Neustart erhalten). */
esp_err_t recorder_set_continuous(bool on);
bool recorder_continuous(void);

rec_reason_t recorder_reason(void);
const char *recorder_reason_text(void);

/** Beendet eine laufende Aufnahme sauber (z. B. vor einem Neustart) und wartet bis zu timeout_ms. */
void recorder_finish(int timeout_ms);

/** Beendet eine laufende Aufnahme (Datei wird sauber abgeschlossen). */
void recorder_stop(void);

/** Erlaubt nach recorder_finish() wieder Aufnahmen (Daueraufnahme laeuft dann weiter). */
void recorder_resume(void);

/** true, wenn SD-Karte und Rekorder bereit sind. */
bool recorder_ready(void);

/** Name der laufenden bzw. letzten Aufnahme ("" = keine). */
const char *recorder_last_file(void);

/** Anzahl Aufnahmen seit dem Start. */
unsigned recorder_count(void);
