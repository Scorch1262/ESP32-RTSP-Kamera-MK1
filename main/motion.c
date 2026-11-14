#include <stdlib.h>
#include <string.h>
#include "motion.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "motion";

#define WARMUP_FRAMES   8
#define HOLD_US         (5LL * 1000 * 1000)

static uint8_t *s_bg;
static int s_w, s_h;
static int s_frames;
static int s_hits;
static int s_level;
static int64_t s_last;

bool motion_feed(const uint8_t *gray, int w, int h, int sens)
{
    int n = w * h;
    if (!s_bg || w != s_w || h != s_h) {
        free(s_bg);
        s_bg = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_bg) {
            return false;
        }
        memcpy(s_bg, gray, n);
        s_w = w;
        s_h = h;
        s_frames = 0;
        return false;
    }
    if (sens < 1) {
        sens = 1;
    } else if (sens > 100) {
        sens = 100;
    }
    /* Schwelle pro Bildpunkt und benoetigter Flaechenanteil aus der Empfindlichkeit */
    int thr = 12 + (100 - sens) * 30 / 100;              /* 12 .. 42 Graustufen */
    int need_pm = 2 + (100 - sens) * 50 / 100;           /* 0,2 % .. 5 % (Promille) */

    int changed = 0;
    for (int i = 0; i < n; i++) {
        int d = gray[i] - s_bg[i];
        if (d < 0) {
            d = -d;
        }
        if (d > thr) {
            changed++;
        }
        /* Hintergrund langsam nachfuehren (1/8) */
        s_bg[i] = (uint8_t)((s_bg[i] * 7 + gray[i] + 4) >> 3);
    }
    s_level = changed * 1000 / n;
    s_frames++;

    if (s_frames < WARMUP_FRAMES) {
        return false;
    }
    if (s_level > 600) {
        /* > 60 % des Bildes veraendert: Lichtwechsel, Hintergrund neu setzen */
        memcpy(s_bg, gray, n);
        s_hits = 0;
        ESP_LOGD(TAG, "Lichtwechsel ignoriert");
        return false;
    }
    if (s_level >= need_pm) {
        s_hits++;
    } else {
        s_hits = 0;
    }
    if (s_hits >= 2) {                                    /* zwei Analysen in Folge */
        s_last = esp_timer_get_time();
        return true;
    }
    return false;
}

int motion_last_level(void)
{
    return s_level;
}

int64_t motion_last_time(void)
{
    return s_last;
}

bool motion_active(void)
{
    return s_last && esp_timer_get_time() - s_last < HOLD_US;
}
