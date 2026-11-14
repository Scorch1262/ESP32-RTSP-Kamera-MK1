/*
 * Ethernet-Initialisierung fuer Waveshare ESP32-P4-ETH
 * Interne EMAC (RMII, 50-MHz-Referenztakt vom PHY an GPIO50), PHY IP101.
 */
#include <string.h>
#include "eth_init.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
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

esp_err_t eth_start(void)
{
    s_evt = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_evt, ESP_ERR_NO_MEM, TAG, "EventGroup");

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "esp_netif_new");
    esp_netif_set_hostname(s_netif, CONFIG_APP_HOSTNAME);

#if CONFIG_APP_USE_STATIC_IP
    esp_netif_dhcpc_stop(s_netif);
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = ipaddr_addr(CONFIG_APP_STATIC_IP);
    ip.netmask.addr = ipaddr_addr(CONFIG_APP_STATIC_NETMASK);
    ip.gw.addr = ipaddr_addr(CONFIG_APP_STATIC_GW);
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_netif, &ip), TAG, "statische IP");
#endif

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

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    static esp_eth_handle_t eth_handle = NULL;
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_config, &eth_handle), TAG, "Treiberinstallation");

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
