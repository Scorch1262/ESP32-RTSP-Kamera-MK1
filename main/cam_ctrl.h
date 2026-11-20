#pragma once
/*
 * Kamera-Einstellungen (Bildregler, Belichtung, Weissabgleich, Autofokus ...).
 *
 * Es werden nur die Regler angeboten, die der angeschlossene Sensor tatsaechlich
 * unterstuetzt. Geaenderte Werte werden sofort uebernommen und im NVS
 * (Namespace "camctl") gespeichert; beim Start werden sie wieder gesetzt.
 * Nur ueber die Webseite einstellbar, nicht per MQTT.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    CAMCTL_RANGE = 0,   /* Schieberegler min..max */
    CAMCTL_BOOL,        /* Ein/Aus */
    CAMCTL_SELECT,      /* Auswahlliste, Werte 0..n-1 */
} cam_ctrl_type_t;

typedef enum {
    CC_BRIGHTNESS = 0,
    CC_CONTRAST,
    CC_SATURATION,
    CC_SHARPNESS,
    CC_DENOISE,
    CC_EFFECT,
    CC_AWB,
    CC_WB_MODE,
    CC_AEC,
    CC_AE_LEVEL,
    CC_AEC_VALUE,
    CC_AGC,
    CC_AGC_GAIN,
    CC_NIGHT,
    CC_LENC,
    CC_BPC,
    CC_WPC,
    CC_GAMMA,
    CC_COLORBAR,
    CC_AF,              /* 0 = aus (Unendlich), 1 = dauerhaft, 2 = einmalig */
    CC_COUNT
} cam_ctrl_id_t;

typedef struct {
    const char *key;          /* NVS-Schluessel und Formularname */
    const char *label;
    const char *group;        /* Ueberschrift auf der Webseite */
    cam_ctrl_type_t type;
    int min, max;
    const char *const *opts;  /* bei CAMCTL_SELECT: max+1 Eintraege */
    const char *hint;         /* optionaler Hinweis */
} cam_ctrl_def_t;

/** Nach erfolgreicher Kamera-Initialisierung aufrufen: erkennt den Sensor und setzt gespeicherte Werte. */
void cam_ctrl_init(void);

/** true, wenn ein Sensor erkannt wurde und Einstellungen moeglich sind. */
bool cam_ctrl_available(void);

/** Name des Sensors (z. B. "OV5640"). */
const char *cam_ctrl_sensor_name(void);

const cam_ctrl_def_t *cam_ctrl_def(cam_ctrl_id_t id);
bool cam_ctrl_supported(cam_ctrl_id_t id);
int  cam_ctrl_get(cam_ctrl_id_t id);
int  cam_ctrl_default(cam_ctrl_id_t id);

/** Schluessel -> ID, -1 wenn unbekannt. */
int cam_ctrl_find(const char *key);

/** Wert setzen, sofort anwenden und speichern. */
esp_err_t cam_ctrl_set(cam_ctrl_id_t id, int value);

/** Alle Werte auf Standard zuruecksetzen (gespeicherte Werte werden geloescht). */
esp_err_t cam_ctrl_reset(void);

/** Autofokus einmal ausloesen (laedt bei Bedarf zuerst die AF-Firmware). */
esp_err_t cam_ctrl_af_trigger(void);

/** Lesbarer Autofokus-Zustand fuer die Webseite. */
void cam_ctrl_af_status(char *out, size_t len);

/** Aktuelle Werte als JSON (fuer /api/kamera). */
void cam_ctrl_json(char *out, size_t len);

/* ---- Schnittstelle zum boardspezifischen Teil (cam_ctrl_p4.c / cam_ctrl_s3.c) ---- */
bool        camb_init(void);                    /* Sensor erkennen */
const char *camb_name(void);
bool        camb_is_ov5640(void);
bool        camb_has(cam_ctrl_id_t id);
int         camb_default(cam_ctrl_id_t id);     /* Ausgangswert nach dem Kamerastart */
esp_err_t   camb_set(cam_ctrl_id_t id, int value);
esp_err_t   camb_reg_write(uint16_t reg, uint8_t val);
esp_err_t   camb_reg_read(uint16_t reg, uint8_t *val);
