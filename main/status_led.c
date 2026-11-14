/*
 * WS2812-Status-LED ueber RMT (Bytes-Encoder, 10 MHz Aufloesung).
 */
#include "status_led.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"

static const char *TAG = "led";

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;

esp_err_t status_led_init(int gpio)
{
    if (gpio < 0) {
        return ESP_OK;
    }
    rmt_tx_channel_config_t cfg = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 48,
        .trans_queue_depth = 2,
    };
    esp_err_t err = rmt_new_tx_channel(&cfg, &s_chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Status-LED nicht verfuegbar: %s", esp_err_to_name(err));
        s_chan = NULL;
        return err;
    }
    rmt_bytes_encoder_config_t ecfg = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },   /* 0,3 us / 0,9 us */
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },   /* 0,9 us / 0,3 us */
        .flags.msb_first = 1,
    };
    rmt_new_bytes_encoder(&ecfg, &s_enc);
    rmt_enable(s_chan);
    return ESP_OK;
}

void status_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_chan) {
        return;
    }
    uint8_t grb[3] = { g, r, b };
    rmt_transmit_config_t tx = { .loop_count = 0 };
    rmt_transmit(s_chan, s_enc, grb, sizeof(grb), &tx);
    rmt_tx_wait_all_done(s_chan, 50);
}
