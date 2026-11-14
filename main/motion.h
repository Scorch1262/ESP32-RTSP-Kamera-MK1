#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * Einfache Bewegungserkennung auf einem kleinen Graustufenbild (z. B. 160x90).
 * Vergleicht mit einem gleitenden Hintergrundbild. Globale Helligkeitsspruenge
 * (IR-Licht an/aus, Wolken) werden als Lichtwechsel erkannt und ignoriert.
 *
 * @param sens   Empfindlichkeit 1..100
 * @return true, wenn in diesem Bild Bewegung erkannt wurde
 */
bool motion_feed(const uint8_t *gray, int w, int h, int sens);

/** Anteil geaenderter Bildpunkte der letzten Analyse in Promille. */
int motion_last_level(void);

/** Zeitpunkt (esp_timer, us) der letzten erkannten Bewegung, 0 = nie. */
int64_t motion_last_time(void);

/** true, solange in den letzten Sekunden Bewegung erkannt wurde. */
bool motion_active(void);
