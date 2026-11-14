/*
 * MicroSD-Karte
 *  - ESP32-S3-ETH: SDMMC 1-Bit (CLK 7, CMD 6, D0 5) ueber die GPIO-Matrix
 *  - ESP32-P4-ETH: SDMMC Slot 0, 4-Bit (CLK 43, CMD 44, D0-D3 39-42), Versorgung ueber LDO-Kanal 4
 */
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include "sdcard.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

static const char *TAG = "sdcard";
static sdmmc_card_t *s_card;

esp_err_t sdcard_mount(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount = {
        .format_if_mount_failed = false,      /* niemals ungefragt formatieren */
        .max_files = 8,
        .allocation_unit_size = 32 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();

#if CONFIG_IDF_TARGET_ESP32P4
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = CONFIG_APP_SD_LDO_CHAN };
    sd_pwr_ctrl_handle_t pwr = NULL;
    if (sd_pwr_ctrl_new_on_chip_ldo(&ldo, &pwr) != ESP_OK) {
        ESP_LOGE(TAG, "SD-Versorgung (LDO %d) konnte nicht eingeschaltet werden", CONFIG_APP_SD_LDO_CHAN);
        return ESP_FAIL;
    }
    host.pwr_ctrl_handle = pwr;
    slot.width = 4;
#else
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    slot.width = 1;
    slot.clk = CONFIG_APP_SD_CLK;
    slot.cmd = CONFIG_APP_SD_CMD;
    slot.d0 = CONFIG_APP_SD_D0;
#endif
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGW(TAG, "Karte gefunden, aber kein FAT-Dateisystem - bitte als FAT32 formatieren");
        } else {
            ESP_LOGW(TAG, "Keine MicroSD-Karte (%s)", esp_err_to_name(err));
        }
        s_card = NULL;
        return err;
    }
    mkdir(SD_REC_DIR, 0775);
    char info[64];
    sdcard_info(info, sizeof(info));
    ESP_LOGI(TAG, "MicroSD-Karte eingebunden: %s", info);
    return ESP_OK;
}

bool sdcard_ok(void)
{
    return s_card != NULL;
}

void sdcard_space(uint64_t *total, uint64_t *free_bytes)
{
    *total = 0;
    *free_bytes = 0;
    if (s_card) {
        esp_vfs_fat_info(SD_MOUNT, total, free_bytes);
    }
}

void sdcard_info(char *out, size_t len)
{
    if (!s_card) {
        snprintf(out, len, "keine Karte");
        return;
    }
    uint64_t t, f;
    sdcard_space(&t, &f);
    snprintf(out, len, "%.1f GB, %.1f GB frei", t / 1e9, f / 1e9);
}
