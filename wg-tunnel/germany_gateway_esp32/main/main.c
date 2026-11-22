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
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "lwip/lwip_napt.h"
#include "dhcpserver/dhcpserver.h"
#include "esp_wireguard.h"
#include "module_api.h"

static const char *TAG = "de_gateway";

#define GOT_IP_BIT BIT0
static EventGroupHandle_t s_events;
static esp_netif_t *s_sta, *s_ap;

/* Statusanzeige (Log + Webseite http://<AP-IP>/) */
static const char *s_stage = "Start";
static char s_ip[20] = "-";
static int s_disc_reason = 0;
static int s_disc_count = 0;
static bool s_time_ok = false;
static bool s_tunnel_up = false;

static const char *reason_text(int r)
{
    switch (r) {
    case 201: return "Router-WLAN nicht gefunden (Name falsch oder nur 5 GHz?)";
    case 202: case 203: case 15: case 204: return "Anmeldung fehlgeschlagen (Passwort falsch?)";
    case 205: return "Verbindung fehlgeschlagen";
    case 2: return "Authentifizierung abgelaufen";
    case 8: return "Router hat getrennt";
    default: return "";
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_stage = "verbinde mit Router-WLAN";
        ESP_LOGI(TAG, "Verbinde mit Router-WLAN \"%s\" ...", CONFIG_STA_SSID);
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        s_disc_reason = d->reason;
        s_disc_count++;
        strcpy(s_ip, "-");
        xEventGroupClearBits(s_events, GOT_IP_BIT);
        ESP_LOGW(TAG, "Router-WLAN NICHT verbunden, Grund %d %s - versuche erneut",
                 d->reason, reason_text(d->reason));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_stage = "WLAN verbunden";
        ESP_LOGI(TAG, "*** Router-WLAN VERBUNDEN, IP: %s ***", s_ip);
        xEventGroupSetBits(s_events, GOT_IP_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Client am SoftAP verbunden");
    }
}

static esp_err_t status_get(httpd_req_t *req)
{
    char buf[1200];
    int n = snprintf(buf, sizeof(buf),
        "<html><head><meta charset=utf-8><meta http-equiv=refresh content=3>"
        "<meta name=viewport content='width=device-width'></head>"
        "<body style='font-family:monospace;font-size:18px'><pre>"
        "DevKit Status\n\n"
        "Phase:           %s\n"
        "Router-WLAN:     %s\n"
        "IP vom Router:   %s\n"
        "Trennungen:      %d (letzter Grund: %d %s)\n"
        "Uhrzeit (NTP):   %s\n"
        "Tunnel zum C3:   %s\n"
        "Laufzeit:        %lld s\n",
        s_stage, CONFIG_STA_SSID, s_ip, s_disc_count, s_disc_reason, reason_text(s_disc_reason),
        s_time_ok ? "ja" : "nein", s_tunnel_up ? "STEHT" : "noch nicht",
        (long long)(esp_timer_get_time() / 1000000));
    if (n < 0 || n >= (int)sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    n += (int)modules_status(buf + n, sizeof(buf) - n); /* Zeilen der Module */
    snprintf(buf + n, sizeof(buf) - n, "</pre></body></html>");
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static void start_status_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) == ESP_OK) {
        httpd_uri_t uri = {.uri = "/", .method = HTTP_GET, .handler = status_get, .user_ctx = NULL};
        httpd_register_uri_handler(srv, &uri);
        ESP_LOGI(TAG, "Statusseite: http://%s/", CONFIG_AP_IP);
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
    modules_run(MODULE_STAGE_EARLY);
    wifi_start();
    start_status_server(); /* laeuft auch, wenn das Router-WLAN nicht klappt */

    ESP_LOGI(TAG, "Warte auf Router-WLAN ...");
    xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    s_stage = "hole Uhrzeit (NTP)";
    if (!sync_time()) {
        s_stage = "KEINE Uhrzeit, Neustart";
        ESP_LOGE(TAG, "Keine NTP-Zeit, Neustart");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }
    s_time_ok = true;
    s_stage = "starte WireGuard";
    modules_run(MODULE_STAGE_NETWORK);

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

    modules_run(MODULE_STAGE_TUNNEL);
    s_stage = "wartet auf C3";
    bool was_up = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        bool up = (esp_wireguardif_peer_is_up(&ctx) == ESP_OK);
        s_tunnel_up = up;
        if (up != was_up) {
            if (up) {
                ESP_LOGI(TAG, "*** TUNNEL ZUM C3 STEHT ***");
                s_stage = "Tunnel steht";
            } else {
                ESP_LOGW(TAG, "Tunnel zum C3 ist unten");
                s_stage = "wartet auf C3";
            }
            was_up = up;
        }
    }
}
