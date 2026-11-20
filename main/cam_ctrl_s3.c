/*
 * Kamera-Einstellungen, ESP32-S3: ueber die Sensorfunktionen von esp32-camera.
 * Angeboten wird, was der erkannte Sensor (OV5640, OV2640, ...) bereitstellt.
 */
#include "esp_camera.h"
#include "sensor.h"
#include "cam_ctrl.h"

static sensor_t *s_s;
static camera_status_t s_init;   /* Zustand direkt nach dem Kamerastart */

bool camb_init(void)
{
    s_s = esp_camera_sensor_get();
    if (!s_s) {
        return false;
    }
    s_init = s_s->status;
    return true;
}

const char *camb_name(void)
{
    camera_sensor_info_t *si = s_s ? esp_camera_sensor_get_info(&s_s->id) : NULL;
    return si ? si->name : "unbekannt";
}

bool camb_is_ov5640(void)
{
    return s_s && s_s->id.PID == OV5640_PID;
}

bool camb_has(cam_ctrl_id_t id)
{
    sensor_t *s = s_s;
    switch (id) {
    case CC_BRIGHTNESS: return s->set_brightness;
    case CC_CONTRAST:   return s->set_contrast;
    case CC_SATURATION: return s->set_saturation;
    case CC_SHARPNESS:  return s->set_sharpness && s->id.PID != OV2640_PID;   /* OV2640: nicht umgesetzt */
    case CC_DENOISE:    return s->set_denoise && s->id.PID == OV5640_PID;
    case CC_EFFECT:     return s->set_special_effect;
    case CC_AWB:        return s->set_whitebal;
    case CC_WB_MODE:    return s->set_wb_mode;
    case CC_AEC:        return s->set_exposure_ctrl;
    case CC_AE_LEVEL:   return s->set_ae_level;
    case CC_AEC_VALUE:  return s->set_aec_value;
    case CC_AGC:        return s->set_gain_ctrl;
    case CC_AGC_GAIN:   return s->set_agc_gain;
    case CC_NIGHT:      return s->set_aec2;
    case CC_LENC:       return s->set_lenc;
    case CC_BPC:        return s->set_bpc;
    case CC_WPC:        return s->set_wpc;
    case CC_GAMMA:      return s->set_raw_gma;
    case CC_COLORBAR:   return s->set_colorbar;
    case CC_AF:         return camb_is_ov5640() && s->set_reg && s->get_reg;
    default:            return false;
    }
}

int camb_default(cam_ctrl_id_t id)
{
    const camera_status_t *st = &s_init;
    switch (id) {
    case CC_BRIGHTNESS: return st->brightness;
    case CC_CONTRAST:   return st->contrast;
    case CC_SATURATION: return st->saturation;
    case CC_SHARPNESS:  return st->sharpness;
    case CC_DENOISE:    return st->denoise;
    case CC_EFFECT:     return st->special_effect;
    case CC_AWB:        return st->awb;
    case CC_WB_MODE:    return st->wb_mode;
    case CC_AEC:        return st->aec;
    case CC_AE_LEVEL:   return st->ae_level;
    case CC_AEC_VALUE:  return st->aec_value;
    case CC_AGC:        return st->agc;
    case CC_AGC_GAIN:   return st->agc_gain;
    case CC_NIGHT:      return st->aec2;
    case CC_LENC:       return st->lenc;
    case CC_BPC:        return st->bpc;
    case CC_WPC:        return st->wpc;
    case CC_GAMMA:      return st->raw_gma;
    case CC_COLORBAR:   return st->colorbar;
    default:            return 0;
    }
}

esp_err_t camb_set(cam_ctrl_id_t id, int v)
{
    sensor_t *s = s_s;
    int r;
    switch (id) {
    case CC_BRIGHTNESS: r = s->set_brightness(s, v); break;
    case CC_CONTRAST:   r = s->set_contrast(s, v); break;
    case CC_SATURATION: r = s->set_saturation(s, v); break;
    case CC_SHARPNESS:  r = s->set_sharpness(s, v); break;
    case CC_DENOISE:    r = s->set_denoise(s, v); break;
    case CC_EFFECT:     r = s->set_special_effect(s, v); break;
    case CC_AWB:        r = s->set_whitebal(s, v); break;
    case CC_WB_MODE:    r = s->set_wb_mode(s, v); break;
    case CC_AEC:        r = s->set_exposure_ctrl(s, v); break;
    case CC_AE_LEVEL:   r = s->set_ae_level(s, v); break;
    case CC_AEC_VALUE:  r = s->set_aec_value(s, v); break;
    case CC_AGC:        r = s->set_gain_ctrl(s, v); break;
    case CC_AGC_GAIN:   r = s->set_agc_gain(s, v); break;
    case CC_NIGHT:      r = s->set_aec2(s, v); break;
    case CC_LENC:       r = s->set_lenc(s, v); break;
    case CC_BPC:        r = s->set_bpc(s, v); break;
    case CC_WPC:        r = s->set_wpc(s, v); break;
    case CC_GAMMA:      r = s->set_raw_gma(s, v); break;
    case CC_COLORBAR:   r = s->set_colorbar(s, v); break;
    default:            return ESP_ERR_NOT_SUPPORTED;
    }
    return r == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t camb_reg_write(uint16_t reg, uint8_t val)
{
    return s_s->set_reg(s_s, reg, 0xff, val) < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t camb_reg_read(uint16_t reg, uint8_t *val)
{
    int v = s_s->get_reg(s_s, reg, 0xff);
    if (v < 0) {
        return ESP_FAIL;
    }
    *val = (uint8_t)v;
    return ESP_OK;
}
