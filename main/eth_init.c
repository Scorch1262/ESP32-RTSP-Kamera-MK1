/*
 * Ethernet-Initialisierung
 *  - Waveshare ESP32-P4-ETH: interne EMAC (RMII, Referenztakt vom PHY an GPIO50), PHY IP101
 *  - Waveshare ESP32-S3-ETH: W5500 am SPI2-Bus (MOSI 11, MISO 12, SCLK 13, CS 14, INT 10, RST 9)
 */
#include <string.h>
#include "eth_init.h"
#include "esp_eth.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_eth_mac_esp.h"
#else
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_mac.h"
#endif
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_check.h"
#include "lwip/ip4_addr.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

static const char *TAG = "eth";

#define ETH_GOT_IP_BIT BIT0

static EventGroupHandle_t s_evt;
static esp_netif_t *s_netif;
static char s_ip[16];

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_eth_handle_t eth = *(esp_eth_handle_t *)data;
    uint8_t mac[6];

    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth, ETH_CMD_G_MAC_ADDR, mac);
        ESP_LOGI(TAG, "Link up  (MAC %02x:%02x:%02x:%02x:%02x:%02x)",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Link down");
        xEventGroupClearBits(s_evt, ETH_GOT_IP_BIT);
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet gestartet");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet gestoppt");
        break;
    default:
        break;
    }
}

static void got_ip_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
    ESP_LOGI(TAG, "IP-Adresse: %s  Maske: " IPSTR "  GW: " IPSTR,
             s_ip, IP2STR(&ev->ip_info.netmask), IP2STR(&ev->ip_info.gw));
    xEventGroupSetBits(s_evt, ETH_GOT_IP_BIT);
}

esp_err_t eth_start(const app_config_t *cfg)
{
    s_evt = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_evt, ESP_ERR_NO_MEM, TAG, "EventGroup");

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "esp_netif_new");
    esp_netif_set_hostname(s_netif, cfg->hostname);

    if (cfg->static_ip) {
        esp_netif_dhcpc_stop(s_netif);
        esp_netif_ip_info_t ip = {0};
        ip.ip.addr = ipaddr_addr(cfg->ip);
        ip.netmask.addr = ipaddr_addr(cfg->netmask);
        ip.gw.addr = ipaddr_addr(cfg->gateway);
        ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_netif, &ip), TAG, "statische IP");
        if (ip.gw.addr) {
            esp_netif_dns_info_t dns = { .ip.u_addr.ip4.addr = ip.gw.addr, .ip.type = ESP_IPADDR_TYPE_V4 };
            esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
        }
        ESP_LOGI(TAG, "Statische IP %s", cfg->ip);
    }

#if CONFIG_IDF_TARGET_ESP32P4
    /* MAC: interne EMAC des ESP32-P4. Die Defaults entsprechen der Waveshare-Belegung
       (TXD0=34, TXD1=35, TX_EN=49, RXD0=29, RXD1=30, CRS_DV=28, REF_CLK-Eingang=50). */
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = CONFIG_APP_ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = CONFIG_APP_ETH_MDIO_GPIO;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    ESP_RETURN_ON_FALSE(mac, ESP_FAIL, TAG, "EMAC konnte nicht angelegt werden");

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_APP_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_APP_ETH_PHY_RST_GPIO;
#if CONFIG_APP_ETH_PHY_IP101
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
#else
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
#endif
    ESP_RETURN_ON_FALSE(phy, ESP_FAIL, TAG, "PHY konnte nicht angelegt werden");

#else
    /* W5500 ueber SPI */
    ESP_RETURN_ON_ERROR(gpio_install_isr_service(0), TAG, "GPIO-ISR-Dienst");
    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_APP_W5500_MOSI,
        .miso_io_num = CONFIG_APP_W5500_MISO,
        .sclk_io_num = CONFIG_APP_W5500_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "SPI-Bus");
    spi_device_interface_config_t dev = {
        .mode = 0,
        .clock_speed_hz = CONFIG_APP_W5500_SPI_MHZ * 1000 * 1000,
        .queue_size = 20,
        .spics_io_num = CONFIG_APP_W5500_CS,
    };
    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &dev);
    w5500_config.int_gpio_num = CONFIG_APP_W5500_INT;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096;
    mac_config.rx_task_prio = 18;
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    ESP_RETURN_ON_FALSE(mac, ESP_FAIL, TAG, "W5500-MAC konnte nicht angelegt werden");

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = CONFIG_APP_W5500_RST;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    ESP_RETURN_ON_FALSE(phy, ESP_FAIL, TAG, "W5500-PHY konnte nicht angelegt werden");

#endif

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    static esp_eth_handle_t eth_handle = NULL;
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_config, &eth_handle), TAG, "Treiberinstallation");
#if !CONFIG_IDF_TARGET_ESP32P4
    /* Der W5500 hat keine eigene MAC-Adresse - die Ethernet-MAC des ESP32-S3 verwenden */
    uint8_t mac_addr[6];
    esp_read_mac(mac_addr, ESP_MAC_ETH);
    esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr);
#endif

    ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif, esp_eth_new_netif_glue(eth_handle)), TAG, "netif attach");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL), TAG, "ETH-Events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_handler, NULL), TAG, "IP-Events");

    return esp_eth_start(eth_handle);
}

bool eth_wait_for_ip(TickType_t timeout, char *ip_out, size_t ip_out_len)
{
    EventBits_t bits = xEventGroupWaitBits(s_evt, ETH_GOT_IP_BIT, pdFALSE, pdTRUE, timeout);
    if ((bits & ETH_GOT_IP_BIT) && ip_out && ip_out_len) {
        strlcpy(ip_out, s_ip, ip_out_len);
    }
    return (bits & ETH_GOT_IP_BIT) != 0;
}
