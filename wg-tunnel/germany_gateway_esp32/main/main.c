/*
 * Deutschland-Gateway (ESP32 DevKit)
 *
 * - STA: verbindet sich mit dem Router in Deutschland (Internetzugang fuer den Tunnel)
 * - SoftAP: eigenes WLAN, in das du deine Geraete einbuchst
 * - WireGuard: wartet auf den C3 aus dem Kosovo (der meldet sich von sich aus)
 * - Der gesamte Verkehr der SoftAP-Clients (und des ESP selbst) wird per
 *   Default-Route in den Tunnel geschickt. Steht der Tunnel nicht, gibt es
 *   KEIN Internet (eingebauter Kill-Switch, kein Leck ueber die deutsche IP).
 */
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "lwip/lwip_napt.h"
#include "esp_wireguard.h"

static const char *TAG = "de_gateway";

#define GOT_IP_BIT BIT0
static EventGroupHandle_t s_events;
static esp_netif_t *s_sta, *s_ap;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, GOT_IP_BIT);
        ESP_LOGW(TAG, "Router-WLAN getrennt, versuche erneut");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP vom Router: " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_events, GOT_IP_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Client am SoftAP verbunden");
    }
}

static void configure_ap_network(void)
{
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr      = esp_ip4addr_aton(CONFIG_AP_IP);
    ip.gw.addr      = ip.ip.addr;
    ip.netmask.addr = esp_ip4addr_aton("255.255.255.0");

    esp_netif_dhcps_stop(s_ap);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap, &ip));

    /* DNS-Server an Clients verteilen; die Anfragen laufen durch den Tunnel. */
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = esp_ip4addr_aton(CONFIG_DNS_SERVER);
    dhcps_offer_t offer = OFFER_DNS;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET,
                                           ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer)));
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_MAIN, &dns));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap));
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap  = esp_netif_create_default_wifi_ap();
    configure_ap_network();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t sta = {0};
    strlcpy((char *)sta.sta.ssid, CONFIG_STA_SSID, sizeof(sta.sta.ssid));
    strlcpy((char *)sta.sta.password, CONFIG_STA_PASSWORD, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = strlen(CONFIG_STA_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;

    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, CONFIG_AP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(CONFIG_AP_SSID);
    ap.ap.max_connection = CONFIG_AP_MAX_CLIENTS;
    if (strlen(CONFIG_AP_PASSWORD) >= 8) {
        strlcpy((char *)ap.ap.password, CONFIG_AP_PASSWORD, sizeof(ap.ap.password));
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap.ap.authmode = WIFI_AUTH_OPEN;
        ESP_LOGW(TAG, "SoftAP ist OFFEN (Passwort leer oder < 8 Zeichen)!");
    }

    /* Der SoftAP uebernimmt automatisch den Funkkanal des Routers. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static bool sync_time(void)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, CONFIG_NTP_SERVER);
    esp_sntp_init();
    for (int i = 0; i < 120; i++) { /* max. 60 s */
        if (time(NULL) > 1700000000) {
            ESP_LOGI(TAG, "Uhrzeit synchronisiert");
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return false;
}

void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    ESP_ERROR_CHECK(r);

    s_events = xEventGroupCreate();
    wifi_start();

    ESP_LOGI(TAG, "Warte auf Router-WLAN ...");
    xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    if (!sync_time()) {
        ESP_LOGE(TAG, "Keine NTP-Zeit, Neustart");
        esp_restart();
    }

    /* WireGuard bindet seinen UDP-Socket an das aktuelle Default-Netif.
     * Das muss die STA-Schnittstelle sein, sonst kaeme der Tunnel-Verkehr
     * nie zum Router. Erst DANACH wird das Tunnel-Interface Default. */
    ESP_ERROR_CHECK(esp_netif_set_default_netif(s_sta));

    static wireguard_config_t cfg = ESP_WIREGUARD_CONFIG_DEFAULT();
    static wireguard_ctx_t ctx = {0};

    cfg.private_key          = CONFIG_WG_PRIVATE_KEY;
    cfg.listen_port          = CONFIG_WG_LISTEN_PORT;
    cfg.public_key           = CONFIG_WG_PEER_PUBLIC_KEY;
    cfg.preshared_key        = strlen(CONFIG_WG_PRESHARED_KEY) ? CONFIG_WG_PRESHARED_KEY : NULL;
    cfg.allowed_ip           = CONFIG_WG_LOCAL_IP;
    cfg.allowed_ip_mask      = CONFIG_WG_LOCAL_NETMASK;
    /* Die Bibliothek verlangt einen Endpoint. Der C3 ist hinter NAT und
     * hat keine feste Adresse; sobald er den ersten Handshake schickt,
     * merkt sich WireGuard seine echte Adresse (Roaming). */
    cfg.endpoint             = CONFIG_WG_PEER_PLACEHOLDER;
    cfg.port                 = CONFIG_WG_LISTEN_PORT;
    cfg.persistent_keepalive = 0;

    ESP_ERROR_CHECK(esp_wireguard_init(&cfg, &ctx));
    if (esp_wireguard_connect(&ctx) != ESP_OK) {
        ESP_LOGE(TAG, "WireGuard connect fehlgeschlagen, Neustart");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    /* NAPT auf der SoftAP-Seite ("innen"): Client-Pakete bekommen beim
     * Eintritt in den Tunnel die Tunnel-IP als Absender. */
    esp_netif_ip_info_t ap_ip;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(s_ap, &ap_ip));
    ip_napt_enable(ap_ip.ip.addr, 1);

    /* Alles Weitere (Clients und der ESP selbst) geht jetzt durch den Tunnel. */
    ESP_ERROR_CHECK(esp_wireguard_set_default(&ctx));
    ESP_LOGI(TAG, "Default-Route = Tunnel. SoftAP \"%s\" (%s) bereit.", CONFIG_AP_SSID, CONFIG_AP_IP);

    bool was_up = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        bool up = (esp_wireguardif_peer_is_up(&ctx) == ESP_OK);
        if (up != was_up) {
            ESP_LOGI(TAG, "Tunnel %s", up ? "STEHT" : "ist unten");
            was_up = up;
        }
    }
}
