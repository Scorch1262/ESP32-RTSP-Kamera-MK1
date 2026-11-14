#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SD_MOUNT   "/sdcard"
#define SD_REC_DIR "/sdcard/rec"

/** Bindet die MicroSD-Karte ein (FAT32). Formatiert NICHT. */
esp_err_t sdcard_mount(void);

bool sdcard_ok(void);

/** Gesamt-/Freispeicher in Byte (0, wenn keine Karte). */
void sdcard_space(uint64_t *total, uint64_t *free_bytes);

/** Kurzinfo fuer die Webseite, z. B. "SDHC 29,7 GB, 12,3 GB frei". */
void sdcard_info(char *out, size_t len);

/** Loescht ALLE Dateien und Ordner auf der Karte (legt /rec neu an). */
esp_err_t sdcard_wipe(int *files, int *dirs, int *errors);
