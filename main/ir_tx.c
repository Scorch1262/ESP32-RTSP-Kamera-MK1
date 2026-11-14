/*
 * IR-Sender ueber RMT mit Hardware-Traegermodulation.
 * Protokolle: NEC (inkl. Extended), Samsung32, Sony SIRC (12/15/20 Bit), RC5, Rohdaten, Pronto-Hex.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <ctype.h>
#include <inttypes.h>
#include "ir_tx.h"
#include "driver/rmt_tx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "cJSON.h"
#include "soc/soc_caps.h"

static const char *TAG = "ir";

#define IR_RES_HZ        1000000     /* 1 Tick = 1 us */
#define IR_MAX_ITEMS     2048
#define IR_DUTY          0.33f
#define RMT_MAX_DUR      32767

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static SemaphoreHandle_t s_lock;
static unsigned s_count;
static int s_rc5_toggle;

/* ------------------------------------------------------------------------- */
/* Puls-Puffer: Folge aus (Pegel, Dauer in us)                                */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t  *level;
    uint32_t *dur;
    int n;
    uint32_t total_us;
    bool overflow;
} pulses_t;

static void p_add(pulses_t *p, int level, uint32_t us)
{
    if (us == 0) {
        return;
    }
    p->total_us += us;
    if (p->n > 0 && p->level[p->n - 1] == level) {
        p->dur[p->n - 1] += us;
        return;
    }
    if (p->n >= IR_MAX_ITEMS) {
        p->overflow = true;
        return;
    }
    p->level[p->n] = level;
    p->dur[p->n] = us;
    p->n++;
}

static inline void mark(pulses_t *p, uint32_t us)
{
    p_add(p, 1, us);
}

static inline void space(pulses_t *p, uint32_t us)
{
    p_add(p, 0, us);
}

/* Fuellt bis zum Ende der Periode (gemessen ab frame_start) mit Pause auf. */
static void space_until(pulses_t *p, uint32_t frame_start, uint32_t period_us, uint32_t min_gap)
{
    uint32_t used = p->total_us - frame_start;
    uint32_t gap = used + min_gap < period_us ? period_us - used : min_gap;
    space(p, gap);
}

/* ------------------------------------------------------------------------- */
/* Protokolle                                                                 */
/* ------------------------------------------------------------------------- */

static void pulse_distance_bits(pulses_t *p, uint32_t data, int bits, uint32_t m, uint32_t s0, uint32_t s1)
{
    for (int i = 0; i < bits; i++) {   /* LSB zuerst */
        mark(p, m);
        space(p, (data >> i) & 1 ? s1 : s0);
    }
}

static void build_nec(pulses_t *p, uint32_t addr, uint32_t cmd, int repeat)
{
    uint32_t start = p->total_us;
    uint32_t a = (addr > 0xFF) ? (addr & 0xFFFF) : ((addr & 0xFF) | ((~addr & 0xFF) << 8));
    uint32_t c = (cmd & 0xFF) | ((~cmd & 0xFF) << 8);
    mark(p, 9000);
    space(p, 4500);
    pulse_distance_bits(p, a | (c << 16), 32, 560, 560, 1690);
    mark(p, 560);
    space_until(p, start, 108000, 20000);
    for (int r = 0; r < repeat; r++) {   /* NEC-Wiederholcode */
        start = p->total_us;
        mark(p, 9000);
        space(p, 2250);
        mark(p, 560);
        space_until(p, start, 108000, 20000);
    }
}

static void build_samsung(pulses_t *p, uint32_t addr, uint32_t cmd, int repeat)
{
    for (int r = 0; r <= repeat; r++) {
        uint32_t start = p->total_us;
        uint32_t a = (addr > 0xFF) ? (addr & 0xFFFF) : ((addr & 0xFF) | ((addr & 0xFF) << 8));
        uint32_t c = (cmd & 0xFF) | ((~cmd & 0xFF) << 8);
        mark(p, 4500);
        space(p, 4500);
        pulse_distance_bits(p, a | (c << 16), 32, 560, 560, 1690);
        mark(p, 560);
        space_until(p, start, 108000, 20000);
    }
}

static void build_sony(pulses_t *p, uint32_t addr, uint32_t cmd, int bits, int repeat)
{
    uint32_t data;
    int n;
    if (bits == 15) {
        data = (cmd & 0x7F) | ((addr & 0xFF) << 7);
        n = 15;
    } else if (bits == 20) {
        data = (cmd & 0x7F) | ((addr & 0x1FFF) << 7);   /* 5 Bit Adresse + 8 Bit Erweiterung */
        n = 20;
    } else {
        data = (cmd & 0x7F) | ((addr & 0x1F) << 7);
        n = 12;
    }
    for (int r = 0; r <= repeat; r++) {
        uint32_t start = p->total_us;
        mark(p, 2400);
        space(p, 600);
        for (int i = 0; i < n; i++) {
            mark(p, (data >> i) & 1 ? 1200 : 600);
            space(p, 600);
        }
        space_until(p, start, 45000, 10000);
    }
}

static void build_rc5(pulses_t *p, uint32_t addr, uint32_t cmd, int toggle, int repeat)
{
    const uint32_t T = 889;
    uint32_t bits = 0;
    bits |= 1u << 13;                                  /* S1 */
    bits |= (cmd & 0x40) ? 0 : (1u << 12);             /* S2 / Feldbit (invertiertes Bit 6) */
    bits |= (toggle & 1) << 11;
    bits |= (addr & 0x1F) << 6;
    bits |= (cmd & 0x3F);
    for (int r = 0; r <= repeat; r++) {
        uint32_t start = p->total_us;
        for (int i = 13; i >= 0; i--) {                /* Manchester, MSB zuerst */
            if ((bits >> i) & 1) {
                space(p, T);
                mark(p, T);
            } else {
                mark(p, T);
                space(p, T);
            }
        }
        space_until(p, start, 113778, 20000);
    }
}

/* ------------------------------------------------------------------------- */
/* Senden                                                                     */
/* ------------------------------------------------------------------------- */

static esp_err_t transmit(pulses_t *p, uint32_t freq)
{
    /* fuehrende Pausen entfernen (RC5 beginnt ggf. mit Pause) */
    int first = 0;
    while (first < p->n && p->level[first] == 0) {
        first++;
    }
    if (first >= p->n) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Halbsymbole zaehlen (lange Dauern werden aufgeteilt) */
    int halves = 0;
    for (int i = first; i < p->n; i++) {
        halves += (p->dur[i] + RMT_MAX_DUR - 1) / RMT_MAX_DUR;
    }
    int nsym = (halves + 1) / 2;
    rmt_symbol_word_t *sym = calloc(nsym, sizeof(rmt_symbol_word_t));
    ESP_RETURN_ON_FALSE(sym, ESP_ERR_NO_MEM, TAG, "Speicher");

    int h = 0;
    for (int i = first; i < p->n; i++) {
        uint32_t d = p->dur[i];
        while (d > 0) {
            uint32_t part = d > RMT_MAX_DUR ? RMT_MAX_DUR : d;
            d -= part;
            rmt_symbol_word_t *s = &sym[h / 2];
            if ((h & 1) == 0) {
                s->level0 = p->level[i];
                s->duration0 = part;
            } else {
                s->level1 = p->level[i];
                s->duration1 = part;
            }
            h++;
        }
    }
    if (h & 1) {                       /* letztes Symbol mit kurzer Pause auffuellen */
        sym[h / 2].level1 = 0;
        sym[h / 2].duration1 = 100;
    }

    rmt_carrier_config_t car = {
        .frequency_hz = freq,
        .duty_cycle = IR_DUTY,
    };
    rmt_disable(s_chan);
    esp_err_t err = rmt_apply_carrier(s_chan, &car);
    rmt_enable(s_chan);
    if (err == ESP_OK) {
        rmt_transmit_config_t tx = { .loop_count = 0 };
        err = rmt_transmit(s_chan, s_enc, sym, nsym * sizeof(rmt_symbol_word_t), &tx);
        if (err == ESP_OK) {
            err = rmt_tx_wait_all_done(s_chan, 5000);
        }
    }
    free(sym);
    return err;
}

/* ------------------------------------------------------------------------- */
/* JSON                                                                       */
/* ------------------------------------------------------------------------- */

static bool get_uint(const cJSON *o, const char *key, uint32_t *out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsNumber(it)) {
        *out = (uint32_t)it->valuedouble;
        return true;
    }
    if (cJSON_IsString(it) && it->valuestring) {
        *out = strtoul(it->valuestring, NULL, 0);   /* "0x07", "7" */
        return true;
    }
    return false;
}

static esp_err_t parse_pronto(pulses_t *p, const char *str, int repeat, uint32_t *freq)
{
    uint16_t w[512];
    int n = 0;
    const char *s = str;
    while (*s && n < 512) {
        while (*s && !isxdigit((unsigned char)*s)) {
            s++;
        }
        if (!*s) {
            break;
        }
        w[n++] = (uint16_t)strtoul(s, (char **)&s, 16);
    }
    if (n < 6 || w[0] != 0x0000 || w[1] == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    float period_us = w[1] * 0.241246f;
    *freq = (uint32_t)(1000000.0f / period_us + 0.5f);
    int n1 = w[2], n2 = w[3];
    if (4 + 2 * (n1 + n2) > n) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint16_t *seq1 = &w[4];
    const uint16_t *seq2 = &w[4 + 2 * n1];
    for (int i = 0; i < 2 * n1; i++) {
        p_add(p, (i & 1) ? 0 : 1, (uint32_t)(seq1[i] * period_us + 0.5f));
    }
    int reps = (n1 == 0) ? repeat + 1 : repeat;
    for (int r = 0; r < reps; r++) {
        for (int i = 0; i < 2 * n2; i++) {
            p_add(p, (i & 1) ? 0 : 1, (uint32_t)(seq2[i] * period_us + 0.5f));
        }
    }
    return ESP_OK;
}

static esp_err_t parse_raw(pulses_t *p, const cJSON *arr, int repeat)
{
    int cnt = cJSON_GetArraySize(arr);
    if (cnt <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    bool has_negative = false;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (cJSON_IsNumber(it) && it->valuedouble < 0) {
            has_negative = true;
        }
    }
    for (int r = 0; r <= repeat; r++) {
        int i = 0;
        cJSON_ArrayForEach(it, arr) {
            if (!cJSON_IsNumber(it)) {
                return ESP_ERR_INVALID_ARG;
            }
            double v = it->valuedouble;
            if (has_negative) {
                p_add(p, v > 0 ? 1 : 0, (uint32_t)(v > 0 ? v : -v));
            } else {
                p_add(p, (i & 1) ? 0 : 1, (uint32_t)v);
            }
            i++;
        }
        if (r < repeat) {
            space(p, 40000);
        }
    }
    return ESP_OK;
}

esp_err_t ir_tx_send_json(const char *json, char *result, size_t result_len)
{
    char dummy[8];
    if (!result) {
        result = dummy;
        result_len = sizeof(dummy);
    }
    result[0] = 0;
    if (!s_chan) {
        snprintf(result, result_len, "IR nicht aktiv (kein GPIO konfiguriert)");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        snprintf(result, result_len, "Ungueltiges JSON");
        return ESP_ERR_INVALID_ARG;
    }

    pulses_t p = {
        .level = malloc(IR_MAX_ITEMS),
        .dur = malloc(IR_MAX_ITEMS * sizeof(uint32_t)),
    };
    esp_err_t err = ESP_OK;
    uint32_t freq = 38000;
    uint32_t addr = 0, cmd = 0, rep = 0, tmp;
    const char *proto = "?";

    if (!p.level || !p.dur) {
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    get_uint(root, "repeat", &rep);
    if (rep > 50) {
        rep = 50;
    }

    const cJSON *jraw = cJSON_GetObjectItemCaseSensitive(root, "raw");
    const cJSON *jpronto = cJSON_GetObjectItemCaseSensitive(root, "pronto");
    const cJSON *jproto = cJSON_GetObjectItemCaseSensitive(root, "protocol");

    if (cJSON_IsArray(jraw)) {
        proto = "RAW";
        err = parse_raw(&p, jraw, rep);
    } else if (cJSON_IsString(jpronto)) {
        proto = "PRONTO";
        err = parse_pronto(&p, jpronto->valuestring, rep, &freq);
    } else if (cJSON_IsString(jproto)) {
        proto = jproto->valuestring;
        bool have_cmd = get_uint(root, "command", &cmd);
        get_uint(root, "address", &addr);
        if (!have_cmd) {
            err = ESP_ERR_INVALID_ARG;
            snprintf(result, result_len, "Feld \"command\" fehlt");
            goto out;
        }
        if (strcasecmp(proto, "NEC") == 0 || strcasecmp(proto, "NECEXT") == 0) {
            build_nec(&p, addr, cmd, rep);
        } else if (strcasecmp(proto, "SAMSUNG") == 0 || strcasecmp(proto, "SAMSUNG32") == 0) {
            build_samsung(&p, addr, cmd, rep);
        } else if (strcasecmp(proto, "SONY") == 0 || strcasecmp(proto, "SIRC") == 0) {
            uint32_t bits = 12;
            get_uint(root, "bits", &bits);
            if (!get_uint(root, "repeat", &tmp)) {
                rep = 2;                         /* Sony: ueblich 3 Sendungen */
            }
            freq = 40000;
            build_sony(&p, addr, cmd, bits, rep);
        } else if (strcasecmp(proto, "RC5") == 0) {
            uint32_t t;
            if (get_uint(root, "toggle", &t)) {
                s_rc5_toggle = t & 1;
            } else {
                s_rc5_toggle ^= 1;
            }
            freq = 36000;
            build_rc5(&p, addr, cmd, s_rc5_toggle, rep);
        } else {
            err = ESP_ERR_NOT_SUPPORTED;
            snprintf(result, result_len, "Unbekanntes Protokoll: %s", proto);
            goto out;
        }
    } else {
        err = ESP_ERR_INVALID_ARG;
        snprintf(result, result_len, "Erwartet \"protocol\", \"raw\" oder \"pronto\"");
        goto out;
    }

    if (get_uint(root, "freq", &tmp) && tmp >= 20000 && tmp <= 60000) {
        freq = tmp;
    }

    if (err == ESP_OK && p.overflow) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        err = transmit(&p, freq);
        xSemaphoreGive(s_lock);
    }
    if (err == ESP_OK) {
        s_count++;
        snprintf(result, result_len, "%s gesendet (%u Pulse, %" PRIu32 " Hz, %" PRIu32 " ms)",
                 proto, p.n, freq, p.total_us / 1000);
        ESP_LOGI(TAG, "%s", result);
    }

out:
    if (err != ESP_OK && result[0] == 0) {
        snprintf(result, result_len, "Fehler: %s", esp_err_to_name(err));
    }
    free(p.level);
    free(p.dur);
    cJSON_Delete(root);
    return err;
}

unsigned ir_tx_count(void)
{
    return s_count;
}

esp_err_t ir_tx_init(int gpio)
{
    if (gpio < 0) {
        ESP_LOGI(TAG, "IR deaktiviert");
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    rmt_tx_channel_config_t cfg = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RES_HZ,
        .mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL,
        .trans_queue_depth = 2,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&cfg, &s_chan), TAG, "RMT-Kanal");
    rmt_copy_encoder_config_t ecfg = {};
    ESP_RETURN_ON_ERROR(rmt_new_copy_encoder(&ecfg, &s_enc), TAG, "Encoder");
    rmt_carrier_config_t car = { .frequency_hz = 38000, .duty_cycle = IR_DUTY };
    ESP_RETURN_ON_ERROR(rmt_apply_carrier(s_chan, &car), TAG, "Traeger");
    ESP_RETURN_ON_ERROR(rmt_enable(s_chan), TAG, "RMT aktivieren");
    ESP_LOGI(TAG, "IR-Sender bereit an GPIO %d", gpio);
    return ESP_OK;
}
