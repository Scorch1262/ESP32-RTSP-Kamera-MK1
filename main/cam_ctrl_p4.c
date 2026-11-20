/*
 * Kamera-Einstellungen, ESP32-P4: direkte Registerzugriffe auf den OV5640 ueber SCCB.
 *
 * Der Kameratreiber (esp_cam_sensor) stellt fuer den OV5640 nur Spiegeln/Kippen bereit.
 * Die Registerfolgen entsprechen denen von espressif/esp32-camera (Apache 2.0).
 * Beim Start werden alle betroffenen Register gesichert; "Standard" stellt genau diesen
 * Ausgangszustand wieder her, damit das Bild ohne Aenderungen unveraendert bleibt.
 */
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_sccb_intf.h"
#include "esp_sccb_i2c.h"
#include "cam_ctrl.h"

static const char *TAG = "camctl";

#define OV5640_ADDR 0x3C
#define OV5640_PID  0x5640

static esp_sccb_io_handle_t s_io;

esp_err_t camb_reg_write(uint16_t reg, uint8_t val)
{
    return esp_sccb_transmit_reg_a16v8(s_io, reg, val);
}

esp_err_t camb_reg_read(uint16_t reg, uint8_t *val)
{
    return esp_sccb_transmit_receive_reg_a16v8(s_io, reg, val);
}

static int rd(uint16_t reg)
{
    uint8_t v;
    return camb_reg_read(reg, &v) == ESP_OK ? v : -1;
}

static esp_err_t wr(uint16_t reg, uint8_t v)
{
    return camb_reg_write(reg, v);
}

static esp_err_t wr16(uint16_t reg, uint16_t v)
{
    esp_err_t e = wr(reg, v >> 8);
    return e == ESP_OK ? wr(reg + 1, v & 0xff) : e;
}

static esp_err_t wr_bits(uint16_t reg, uint8_t mask, bool on)
{
    int v = rd(reg);
    if (v < 0) {
        return ESP_FAIL;
    }
    return wr(reg, on ? (v | mask) : (v & ~mask));
}

/* ---- Sicherung des Ausgangszustands ---- */

static const uint16_t s_backup_regs[] = {
    0x5000, 0x5001, 0x5003, 0x503d,
    0x5580, 0x5583, 0x5584, 0x5586, 0x5587, 0x5588,
    0x5381, 0x5382, 0x5383, 0x5384, 0x5385, 0x5386, 0x5387, 0x5388, 0x5389, 0x538a, 0x538b,
    0x5300, 0x5301, 0x5302, 0x5303, 0x5306, 0x5308, 0x5309, 0x530a, 0x530b, 0x530c,
    0x3400, 0x3401, 0x3402, 0x3403, 0x3404, 0x3405, 0x3406,
    0x3503, 0x3a00, 0x3a0f, 0x3a10, 0x3a11, 0x3a1b, 0x3a1e, 0x3a1f,
    0x3500, 0x3501, 0x3502, 0x350a, 0x350b,
};
#define N_BACKUP (sizeof(s_backup_regs) / sizeof(s_backup_regs[0]))
static uint8_t s_backup[N_BACKUP];

static int orig(uint16_t reg)
{
    for (size_t i = 0; i < N_BACKUP; i++) {
        if (s_backup_regs[i] == reg) {
            return s_backup[i];
        }
    }
    return -1;
}

static esp_err_t restore(const uint16_t *regs, size_t n)
{
    esp_err_t e = ESP_OK;
    for (size_t i = 0; i < n && e == ESP_OK; i++) {
        int v = orig(regs[i]);
        if (v >= 0) {
            e = wr(regs[i], v);
        }
    }
    return e;
}

/* ---- Tabellen (aus esp32-camera, sensors/private_include/ov5640_settings.h) ---- */

static const uint8_t s_saturation[5][11] = {
    {0x1d, 0x60, 0x03, 0x0a, 0x60, 0x6a, 0x64, 0x56, 0x0e, 0x01, 0x98}, /* -2 */
    {0x1d, 0x60, 0x03, 0x0b, 0x6c, 0x77, 0x70, 0x60, 0x10, 0x01, 0x98}, /* -1 */
    {0x1d, 0x60, 0x03, 0x0c, 0x78, 0x84, 0x7d, 0x6b, 0x12, 0x01, 0x98}, /*  0 */
    {0x1d, 0x60, 0x03, 0x0d, 0x84, 0x91, 0x8a, 0x76, 0x14, 0x01, 0x98}, /* +1 */
    {0x1d, 0x60, 0x03, 0x0e, 0x90, 0x9e, 0x96, 0x80, 0x16, 0x01, 0x98}, /* +2 */
};

/* SDE 0x5580, 0x5583, 0x5584. "Normal"/"Negativ" ohne Saettigungs-Bit, damit die Farben
   des Ausgangszustands erhalten bleiben; Toenungen mit festen U/V-Werten. */
static const uint8_t s_effect[7][3] = {
    {0x04, 0x40, 0x40},   /* Normal */
    {0x44, 0x40, 0x40},   /* Negativ */
    {0x1c, 0x80, 0x80},   /* Graustufen */
    {0x1c, 0x80, 0xc0},   /* Rot */
    {0x1c, 0x60, 0x60},   /* Gruen */
    {0x1c, 0xa0, 0x40},   /* Blau */
    {0x1c, 0x40, 0xa0},   /* Sepia */
};

/* Aktuelle Werte der gemeinsam wirkenden SDE-Regler */
static int s_bri, s_con, s_eff;

static esp_err_t sde_apply(void)
{
    static const uint16_t sde_regs[] = { 0x5001, 0x5580, 0x5583, 0x5584, 0x5586, 0x5587, 0x5588 };
    if (s_bri == 0 && s_con == 0 && s_eff == 0) {
        return restore(sde_regs, sizeof(sde_regs) / sizeof(sde_regs[0]));
    }
    int bright = s_bri < 0 ? -s_bri : s_bri;
    esp_err_t e = wr(0x5580, s_effect[s_eff][0]);
    if (e == ESP_OK) e = wr(0x5583, s_effect[s_eff][1]);
    if (e == ESP_OK) e = wr(0x5584, s_effect[s_eff][2]);
    if (e == ESP_OK) e = wr(0x5586, (s_con + 4) << 3);
    if (e == ESP_OK) e = wr(0x5587, bright << 4);
    if (e == ESP_OK) e = wr_bits(0x5588, 0x08, s_bri < 0);
    if (e == ESP_OK) e = wr_bits(0x5001, 0x80, true);     /* SDE einschalten */
    return e;
}

/* ---- Schnittstelle ---- */

bool camb_init(void)
{
    i2c_master_bus_handle_t bus;
    if (i2c_master_get_bus_handle(CONFIG_APP_CAM_SCCB_PORT, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "SCCB-Bus nicht gefunden");
        return false;
    }
    sccb_i2c_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OV5640_ADDR,
        .scl_speed_hz = CONFIG_APP_CAM_SCCB_FREQ,
        .addr_bits_width = 16,
        .val_bits_width = 8,
    };
    if (sccb_new_i2c_io(bus, &cfg, &s_io) != ESP_OK) {
        return false;
    }
    int h = rd(0x300a), l = rd(0x300b);
    if (h < 0 || l < 0 || ((h << 8) | l) != OV5640_PID) {
        ESP_LOGW(TAG, "Kein OV5640 erkannt (PID 0x%02x%02x) - keine Kamera-Einstellungen", h & 0xff, l & 0xff);
        esp_sccb_del_i2c_io(s_io);
        s_io = NULL;
        return false;
    }
    for (size_t i = 0; i < N_BACKUP; i++) {
        int v = rd(s_backup_regs[i]);
        s_backup[i] = v < 0 ? 0 : v;
    }
    return true;
}

const char *camb_name(void)
{
    return "OV5640";
}

bool camb_is_ov5640(void)
{
    return true;
}

bool camb_has(cam_ctrl_id_t id)
{
    return id < CC_COUNT;
}

int camb_default(cam_ctrl_id_t id)
{
    switch (id) {
    case CC_AWB:      return (orig(0x5001) & 0x01) != 0;
    case CC_AEC:      return (orig(0x3503) & 0x01) == 0;
    case CC_AGC:      return (orig(0x3503) & 0x02) == 0;
    case CC_NIGHT:    return (orig(0x3a00) & 0x04) != 0;
    case CC_LENC:     return (orig(0x5000) & 0x80) != 0;
    case CC_BPC:      return (orig(0x5000) & 0x04) != 0;
    case CC_WPC:      return (orig(0x5000) & 0x02) != 0;
    case CC_GAMMA:    return (orig(0x5000) & 0x20) != 0;
    case CC_COLORBAR: return (orig(0x503d) & 0x80) != 0;
    case CC_DENOISE:
        return (orig(0x5308) & 0x10) ? orig(0x5306) / 4 + 1 : 0;
    case CC_AEC_VALUE:
        return (orig(0x3500) & 0x0f) << 12 | orig(0x3501) << 4 | (orig(0x3502) & 0xf0) >> 4;
    case CC_AGC_GAIN: {
        int ra = orig(0x350a), rb = orig(0x350b);
        int g = (rb & 0xf0) >> 4 | (ra & 0x03) << 4;
        return (rb & 0x0f) ? g + 1 : g;
    }
    default:
        return 0;
    }
}

esp_err_t camb_set(cam_ctrl_id_t id, int v)
{
    switch (id) {
    case CC_BRIGHTNESS:
        s_bri = v;
        return sde_apply();
    case CC_CONTRAST:
        s_con = v;
        return sde_apply();
    case CC_EFFECT:
        s_eff = v;
        return sde_apply();

    case CC_SATURATION: {
        static const uint16_t r[] = { 0x5381, 0x5382, 0x5383, 0x5384, 0x5385, 0x5386, 0x5387, 0x5388,
                                      0x5389, 0x538a, 0x538b };
        if (v == 0) {
            return restore(r, 11);
        }
        esp_err_t e = ESP_OK;
        for (int i = 0; i < 11 && e == ESP_OK; i++) {
            e = wr(0x5381 + i, s_saturation[v + 2][i]);
        }
        return e;
    }

    case CC_SHARPNESS: {
        static const uint16_t r[] = { 0x5300, 0x5301, 0x5302, 0x5303, 0x5308, 0x5309, 0x530a, 0x530b, 0x530c };
        if (v == 0) {
            return restore(r, sizeof(r) / sizeof(r[0]));
        }
        uint8_t off2 = (v + 3) * 8;
        esp_err_t e = wr_bits(0x5308, 0x40, false);       /* manuelle Schaerfe */
        if (e == ESP_OK) e = wr(0x5300, 0x10);
        if (e == ESP_OK) e = wr(0x5301, 0x10);
        if (e == ESP_OK) e = wr(0x5302, off2 + 1);
        if (e == ESP_OK) e = wr(0x5303, off2);
        if (e == ESP_OK) e = wr(0x5309, 0x10);
        if (e == ESP_OK) e = wr(0x530a, 0x10);
        if (e == ESP_OK) e = wr(0x530b, 0x04);
        if (e == ESP_OK) e = wr(0x530c, 0x06);
        return e;
    }

    case CC_DENOISE:
        if (v == camb_default(CC_DENOISE)) {
            static const uint16_t r[] = { 0x5306, 0x5308 };
            return restore(r, 2);
        } else {
            esp_err_t e = wr_bits(0x5308, 0x10, v > 0);
            if (e == ESP_OK && v > 0) {
                e = wr(0x5306, (v - 1) * 4);
            }
            return e;
        }

    case CC_AWB:
        return wr_bits(0x5001, 0x01, v);

    case CC_WB_MODE: {
        static const uint16_t gains[5][3] = {
            {0, 0, 0}, {0x5e0, 0x410, 0x540}, {0x650, 0x410, 0x4f0}, {0x520, 0x410, 0x660}, {0x420, 0x3f0, 0x710},
        };
        if (v == 0) {
            static const uint16_t r[] = { 0x3400, 0x3401, 0x3402, 0x3403, 0x3404, 0x3405, 0x3406 };
            return restore(r, 7);
        }
        esp_err_t e = wr(0x3406, 0x01);                 /* manuelle AWB-Verstaerkung */
        if (e == ESP_OK) e = wr16(0x3400, gains[v][0]);
        if (e == ESP_OK) e = wr16(0x3402, gains[v][1]);
        if (e == ESP_OK) e = wr16(0x3404, gains[v][2]);
        return e;
    }

    case CC_AEC:
        return wr_bits(0x3503, 0x01, !v);
    case CC_AGC:
        return wr_bits(0x3503, 0x02, !v);

    case CC_AE_LEVEL: {
        static const uint16_t r[] = { 0x3a0f, 0x3a10, 0x3a11, 0x3a1b, 0x3a1e, 0x3a1f };
        if (v == 0) {
            return restore(r, 6);
        }
        int target = (v + 5) * 10 + 5;
        int lo = target * 23 / 25, hi = target * 27 / 25;
        int fhi = hi << 1 > 255 ? 255 : hi << 1;
        esp_err_t e = wr(0x3a0f, hi);
        if (e == ESP_OK) e = wr(0x3a10, lo);
        if (e == ESP_OK) e = wr(0x3a1b, hi);
        if (e == ESP_OK) e = wr(0x3a1e, lo);
        if (e == ESP_OK) e = wr(0x3a11, fhi);
        if (e == ESP_OK) e = wr(0x3a1f, lo >> 1);
        return e;
    }

    case CC_AEC_VALUE: {
        int vh = rd(0x380e), vl = rd(0x380f);
        if (vh >= 0 && vl >= 0) {
            int vts = (vh << 8) | vl;
            if (v > vts) {
                v = vts;
            }
        }
        esp_err_t e = wr(0x3500, (v >> 12) & 0x0f);
        if (e == ESP_OK) e = wr(0x3501, (v >> 4) & 0xff);
        if (e == ESP_OK) e = wr(0x3502, (v << 4) & 0xf0);
        return e;
    }

    case CC_AGC_GAIN: {
        int g = v << 4;
        if (g) {
            g -= 1;
        }
        esp_err_t e = wr(0x350a, (g >> 8) & 0x03);
        if (e == ESP_OK) e = wr(0x350b, g & 0xff);
        return e;
    }

    case CC_NIGHT:    return wr_bits(0x3a00, 0x04, v);
    case CC_LENC:     return wr_bits(0x5000, 0x80, v);
    case CC_BPC:      return wr_bits(0x5000, 0x04, v);
    case CC_WPC:      return wr_bits(0x5000, 0x02, v);
    case CC_GAMMA:    return wr_bits(0x5000, 0x20, v);
    case CC_COLORBAR: return wr_bits(0x503d, 0x80, v);
    default:          return ESP_ERR_NOT_SUPPORTED;
    }
}
