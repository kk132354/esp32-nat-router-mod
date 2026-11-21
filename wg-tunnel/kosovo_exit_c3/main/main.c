/*
 * Kosovo Exit-Node (ESP32-C3)
 *
 * - verbindet sich als WLAN-Client mit dem Router im Kosovo
 * - baut einen WireGuard-Tunnel zum Deutschland-Gateway auf (C3 ist Initiator,
 *   dadurch reicht im Kosovo ein normaler NAT-Router ohne Portfreigabe)
 * - leitet alles, was durch den Tunnel kommt, per NAPT ins Internet
 *   (die Ziel-Server sehen die oeffentliche IP des Kosovo-Anschlusses)
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

static const char *TAG = "kosovo_exit";

#define GOT_IP_BIT BIT0
static EventGroupHandle_t s_events;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        xEventGroupClearBits(s_events, GOT_IP_BIT);
        ESP_LOGW(TAG, "WLAN NICHT verbunden, Grund %d (201=nicht gefunden/5GHz?, 202/15=Passwort?) - versuche erneut",
                 d->reason);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "*** WLAN VERBUNDEN, IP: " IPSTR " ***", IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_events, GOT_IP_BIT);
    }
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, CONFIG_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, CONFIG_WIFI_PASSWORD, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = strlen(CONFIG_WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE)); /* Dauerbetrieb, volle Leistung */
    ESP_ERROR_CHECK(esp_wifi_start());
}

static bool sync_time(void)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, CONFIG_NTP_SERVER);
    esp_sntp_init();
    for (int i = 0; i < 120; i++) { /* max. 60 s */
        time_t now = time(NULL);
        if (now > 1700000000) {
            ESP_LOGI(TAG, "Uhrzeit synchronisiert: %lld", (long long)now);
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

    ESP_LOGI(TAG, "Warte auf WLAN ...");
    xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    if (!sync_time()) {
        ESP_LOGE(TAG, "Keine NTP-Zeit, Neustart");
        esp_restart();
    }

    /* WireGuard bindet sich an das beim Init aktive Default-Netif (= STA).
     * Das Default-Netif wird hier bewusst NICHT veraendert: Ausgehender Verkehr
     * des Tunnels geht ueber das WLAN zum Kosovo-Router. */
    static wireguard_config_t cfg = ESP_WIREGUARD_CONFIG_DEFAULT();
    static wireguard_ctx_t ctx = {0};

    cfg.private_key          = CONFIG_WG_PRIVATE_KEY;
    cfg.listen_port          = CONFIG_WG_LOCAL_PORT;
    cfg.public_key           = CONFIG_WG_PEER_PUBLIC_KEY;
    cfg.preshared_key        = strlen(CONFIG_WG_PRESHARED_KEY) ? CONFIG_WG_PRESHARED_KEY : NULL;
    cfg.allowed_ip           = CONFIG_WG_LOCAL_IP;
    cfg.allowed_ip_mask      = CONFIG_WG_LOCAL_NETMASK;
    cfg.endpoint             = CONFIG_WG_PEER_ENDPOINT;
    cfg.port                 = CONFIG_WG_PEER_PORT;
    cfg.persistent_keepalive = CONFIG_WG_KEEPALIVE;

    ESP_ERROR_CHECK(esp_wireguard_init(&cfg, &ctx));
    if (esp_wireguard_connect(&ctx) != ESP_OK) {
        ESP_LOGE(TAG, "WireGuard connect fehlgeschlagen (Endpoint/DNS?), Neustart");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_restart();
    }

    /* NAPT auf der Tunnelseite ("innen"): Pakete aus dem Tunnel werden beim
     * Verlassen ueber das WLAN auf die Router-IP umgeschrieben. */
    ip_napt_enable(esp_ip4addr_aton(CONFIG_WG_LOCAL_IP), 1);
    ESP_LOGI(TAG, "NAPT aktiv auf Tunnel-Interface %s", CONFIG_WG_LOCAL_IP);

    /* Ueberwachung: steht der Tunnel laenger nicht, hilft ein Neustart
     * (neue DDNS-Aufloesung, neues WLAN, neuer Handshake). */
    int down_s = 0;
    bool was_up = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        bool up = (esp_wireguardif_peer_is_up(&ctx) == ESP_OK);
        if (up != was_up) {
            ESP_LOGI(TAG, "%s", up ? "*** TUNNEL ZUM DEVKIT STEHT ***" : "Tunnel zum DevKit ist unten");
            was_up = up;
        }
        down_s = up ? 0 : down_s + 5;
        if (down_s >= CONFIG_TUNNEL_DOWN_RESTART_S) {
            ESP_LOGE(TAG, "Tunnel seit %d s unten, Neustart", down_s);
            esp_restart();
        }
    }
}
