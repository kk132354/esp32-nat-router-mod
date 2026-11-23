/*
 * Web-Grundlage: Seitenlayout, Anmeldung per Passwort (Cookie), Seite "Sicherheit".
 *
 * Regeln
 *  - Ansehen darf jeder, der die Seite erreicht.
 *  - Kein Passwort gesetzt: Aendern nur aus vertrauten Netzen (SoftAP, Tunnel, optional Heimnetz).
 *  - Passwort gesetzt: Aendern nur nach Anmeldung (Cookie), von ueberall.
 *  - Passwort setzen geht nur, solange keins gesetzt ist, und nur aus vertrauten Netzen.
 *  - Passwort vergessen: BOOT-Taste am Geraet 5 Sekunden halten (loescht das Passwort).
 *  - Das Passwort wird nur als gesalzener SHA-256-Hash gespeichert.
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include "module_api.h"
#include "web.h"

static const char *TAG = "web";

#define COOKIE_NAME "sid_" CONFIG_IDF_TARGET
#define SESS_MAX 4

static char s_sess[SESS_MAX][33];
static int s_sess_next = 0;
static bool s_pw_set = false;
static uint8_t s_salt[16];
static uint8_t s_hash[32];
static int s_fails = 0;
static int64_t s_lock_until_us = 0;

/* ---------- Passwort ---------- */

static void hash_pw(const char *pw, const uint8_t *salt, uint8_t *out)
{
    uint8_t buf[16 + 64];
    size_t n = strnlen(pw, 64);
    memcpy(buf, salt, 16);
    memcpy(buf + 16, pw, n);
    mbedtls_sha256(buf, 16 + n, out, 0);
}

static bool check_pw(const char *pw)
{
    uint8_t h[32];
    hash_pw(pw, s_salt, h);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) {
        diff |= (uint8_t)(h[i] ^ s_hash[i]);
    }
    return s_pw_set && diff == 0;
}

static void load_password(void)
{
    nvs_handle_t h;
    if (nvs_open("web", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t l1 = sizeof(s_salt), l2 = sizeof(s_hash);
    if (nvs_get_blob(h, "salt", s_salt, &l1) == ESP_OK && l1 == sizeof(s_salt) &&
        nvs_get_blob(h, "hash", s_hash, &l2) == ESP_OK && l2 == sizeof(s_hash)) {
        s_pw_set = true;
    }
    nvs_close(h);
}

static void clear_sessions(void)
{
    memset(s_sess, 0, sizeof(s_sess));
}

static void set_password(const char *pw)
{
    esp_fill_random(s_salt, sizeof(s_salt));
    hash_pw(pw, s_salt, s_hash);
    nvs_handle_t h;
    if (nvs_open("web", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "salt", s_salt, sizeof(s_salt));
        nvs_set_blob(h, "hash", s_hash, sizeof(s_hash));
        nvs_commit(h);
        nvs_close(h);
    }
    s_pw_set = true;
    clear_sessions();
}

static void clear_password(void)
{
    nvs_handle_t h;
    if (nvs_open("web", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "salt");
        nvs_erase_key(h, "hash");
        nvs_commit(h);
        nvs_close(h);
    }
    s_pw_set = false;
    clear_sessions();
}

/* ---------- Sitzungen ---------- */

static void new_session(char *tok_out /* 33 Bytes */)
{
    uint8_t r[16];
    esp_fill_random(r, sizeof(r));
    for (int i = 0; i < 16; i++) {
        sprintf(tok_out + 2 * i, "%02x", r[i]);
    }
    memcpy(s_sess[s_sess_next], tok_out, 33);
    s_sess_next = (s_sess_next + 1) % SESS_MAX;
}

static bool cookie_token(httpd_req_t *req, char *tok /* 33 Bytes */)
{
    char ck[320];
    if (httpd_req_get_hdr_value_str(req, "Cookie", ck, sizeof(ck)) != ESP_OK) {
        return false;
    }
    const char *p = strstr(ck, COOKIE_NAME "=");
    if (!p) {
        return false;
    }
    p += strlen(COOKIE_NAME) + 1;
    size_t i = 0;
    while (i < 32 && isxdigit((unsigned char)p[i])) {
        tok[i] = p[i];
        i++;
    }
    tok[i] = '\0';
    return i == 32;
}

static bool session_valid(httpd_req_t *req)
{
    char tok[33];
    if (!cookie_token(req, tok)) {
        return false;
    }
    for (int k = 0; k < SESS_MAX; k++) {
        if (s_sess[k][0] && strcmp(s_sess[k], tok) == 0) {
            return true;
        }
    }
    return false;
}

static void drop_session(httpd_req_t *req)
{
    char tok[33];
    if (!cookie_token(req, tok)) {
        return;
    }
    for (int k = 0; k < SESS_MAX; k++) {
        if (strcmp(s_sess[k], tok) == 0) {
            s_sess[k][0] = '\0';
        }
    }
}

/* ---------- Herkunft der Anfrage ---------- */

static bool peer_ip(httpd_req_t *req, uint32_t *ip_nbo)
{
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage ss;
    socklen_t l = sizeof(ss);
    if (fd < 0 || getpeername(fd, (struct sockaddr *)&ss, &l) != 0) {
        return false;
    }
    if (ss.ss_family == AF_INET) {
        *ip_nbo = ((struct sockaddr_in *)&ss)->sin_addr.s_addr;
        return true;
    }
    if (ss.ss_family == AF_INET6) {
        const uint8_t *b = (const uint8_t *)&((struct sockaddr_in6 *)&ss)->sin6_addr;
        static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (memcmp(b, mapped, 12) != 0) {
            return false;
        }
        memcpy(ip_nbo, b + 12, 4);
        return true;
    }
    return false;
}

static bool in_netif_subnet(const char *ifkey, uint32_t ip)
{
    esp_netif_t *n = esp_netif_get_handle_from_ifkey(ifkey);
    esp_netif_ip_info_t i;
    if (!n || esp_netif_get_ip_info(n, &i) != ESP_OK || i.ip.addr == 0) {
        return false;
    }
    return (ip & i.netmask.addr) == (i.ip.addr & i.netmask.addr);
}

bool web_trusted(httpd_req_t *req)
{
    uint32_t ip;
    if (!peer_ip(req, &ip)) {
        return false;
    }
    uint32_t tn = esp_ip4addr_aton(CONFIG_WG_LOCAL_IP);
    uint32_t tm = esp_ip4addr_aton(CONFIG_WG_LOCAL_NETMASK);
    if ((ip & tm) == (tn & tm)) {
        /* Aus dem Tunnel: Die Gegenstelle gibt per Header an, ob ihr Besucher vertraut ist. */
        char v[4];
        if (httpd_req_get_hdr_value_str(req, "X-Peer-Trusted", v, sizeof(v)) == ESP_OK) {
            return v[0] == '1';
        }
        return true;
    }
#ifdef CONFIG_WEB_TRUST_AP
    if (in_netif_subnet("WIFI_AP_DEF", ip)) {
        return true;
    }
#endif
#ifdef CONFIG_WEB_TRUST_LAN
    if (in_netif_subnet("WIFI_STA_DEF", ip)) {
        return true;
    }
#endif
    return false;
}

bool web_password_set(void) { return s_pw_set; }

bool web_can_control(httpd_req_t *req)
{
    return s_pw_set ? session_valid(req) : web_trusted(req);
}

/* ---------- Formulare ---------- */

static void url_decode(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '+') {
            *o++ = ' ';
            s++;
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            s += 3;
        } else {
            *o++ = *s++;
        }
    }
    *o = '\0';
}

char *web_read_body(httpd_req_t *req)
{
    size_t n = req->content_len;
    if (n == 0 || n > 600) {
        return NULL;
    }
    char *b = malloc(n + 1);
    if (!b) {
        return NULL;
    }
    size_t got = 0;
    while (got < n) {
        int r = httpd_req_recv(req, b + got, n - got);
        if (r <= 0) {
            free(b);
            return NULL;
        }
        got += (size_t)r;
    }
    b[n] = '\0';
    return b;
}

bool web_form_get(const char *body, const char *key, char *out, size_t len)
{
    char raw[200];
    if (!body || httpd_query_key_value(body, key, raw, sizeof(raw)) != ESP_OK) {
        return false;
    }
    url_decode(raw);
    strlcpy(out, raw, len);
    return true;
}

/* ---------- Seitenlayout ---------- */

esp_err_t web_send_page(httpd_req_t *req, const char *title, const char *body_html)
{
    char tmp[16];
    bool proxied = httpd_req_get_hdr_value_str(req, "X-Proxied", tmp, sizeof(tmp)) == ESP_OK;
    char nav[600];
    modules_nav_html(nav, sizeof(nav));

    const char *who;
    if (session_valid(req)) {
        who = "angemeldet &middot; <a href=\"logout\">Abmelden</a>";
    } else if (s_pw_set) {
        who = "nicht angemeldet &middot; <a href=\"login\">Anmelden</a>";
    } else {
        who = "kein Passwort (offen)";
    }

    char head[1500];
    snprintf(head, sizeof(head),
        "<!doctype html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'><title>%s</title>"
        "<style>body{font-family:sans-serif;font-size:18px;margin:12px}"
        "nav a{margin-right:12px}pre{background:#eee;padding:8px;overflow:auto}"
        "input,select,button{font-size:18px;margin:4px 0}.msg{color:#b00}"
        ".dev{background:#dde;padding:4px 8px}</style></head><body>"
        "<div class=dev>Geraet: <b>%s</b> &middot; %s</div>"
        "<nav><a href=\"./\">Start</a> %s<a href=\"security\">Sicherheit</a> %s</nav>"
        "<hr><h2>%s</h2>",
        title, CONFIG_IDF_TARGET, who, nav,
        proxied ? "<a href=\"/\">&laquo; zurueck zum anderen Geraet</a>" : "<a href=\"/peer/\">Gegenstelle</a>",
        title);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, body_html, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(req, "</body></html>", HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ---------- Seiten: Anmelden, Abmelden, Sicherheit ---------- */

static esp_err_t login_page(httpd_req_t *req)
{
    const char *msg = "";
    char ck[130] = "";

    if (req->method == HTTP_POST) {
        char *body = web_read_body(req);
        char pw[48] = "";
        if (body) {
            web_form_get(body, "pw", pw, sizeof(pw));
            free(body);
        }
        int64_t now = esp_timer_get_time();
        if (!s_pw_set) {
            msg = "Es ist kein Passwort gesetzt.";
        } else if (now < s_lock_until_us) {
            msg = "Zu viele Fehlversuche. Bitte eine Minute warten.";
        } else if (check_pw(pw)) {
            char tok[33];
            s_fails = 0;
            new_session(tok);
            snprintf(ck, sizeof(ck), COOKIE_NAME "=%s; Path=/; HttpOnly; SameSite=Strict", tok);
            httpd_resp_set_hdr(req, "Set-Cookie", ck);
            msg = "Angemeldet.";
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (++s_fails >= 5) {
                s_fails = 0;
                s_lock_until_us = now + 60LL * 1000000LL;
            }
            msg = "Falsches Passwort.";
        }
    }

    char body[500];
    snprintf(body, sizeof(body),
        "<p class=msg>%s</p>"
        "<form method=post action=login>Passwort:<br><input name=pw type=password><br>"
        "<button type=submit>Anmelden</button></form>", msg);
    return web_send_page(req, "Anmelden", body);
}

static esp_err_t logout_page(httpd_req_t *req)
{
    drop_session(req);
    httpd_resp_set_hdr(req, "Set-Cookie", COOKIE_NAME "=; Path=/; Max-Age=0");
    return web_send_page(req, "Abgemeldet", "<p>Du bist abgemeldet.</p>");
}

static esp_err_t security_page(httpd_req_t *req)
{
    const char *msg = "";
    char ck[130] = "";

    if (req->method == HTTP_POST) {
        char *body = web_read_body(req);
        char act[12] = "", p1[48] = "", p2[48] = "", cur[48] = "";
        if (body) {
            web_form_get(body, "action", act, sizeof(act));
            web_form_get(body, "p1", p1, sizeof(p1));
            web_form_get(body, "p2", p2, sizeof(p2));
            web_form_get(body, "cur", cur, sizeof(cur));
            free(body);
        }
        bool authed = session_valid(req);
        bool want_new = !strcmp(act, "set") || !strcmp(act, "change");

        if (want_new && strlen(p1) < 4) {
            msg = "Das Passwort braucht mindestens 4 Zeichen.";
        } else if (want_new && strcmp(p1, p2) != 0) {
            msg = "Die beiden Eingaben sind verschieden.";
        } else if (!strcmp(act, "set")) {
            if (s_pw_set) {
                msg = "Es ist schon ein Passwort gesetzt.";
            } else if (!web_trusted(req)) {
                msg = "Aus diesem Netz ist das Setzen nicht erlaubt.";
            } else {
                set_password(p1);
                char tok[33];
                new_session(tok);
                snprintf(ck, sizeof(ck), COOKIE_NAME "=%s; Path=/; HttpOnly; SameSite=Strict", tok);
                httpd_resp_set_hdr(req, "Set-Cookie", ck);
                msg = "Passwort gesetzt. Du bist angemeldet.";
            }
        } else if (!strcmp(act, "change")) {
            if (!s_pw_set || !authed) {
                msg = "Bitte zuerst anmelden.";
            } else {
                set_password(p1);
                char tok[33];
                new_session(tok);
                snprintf(ck, sizeof(ck), COOKIE_NAME "=%s; Path=/; HttpOnly; SameSite=Strict", tok);
                httpd_resp_set_hdr(req, "Set-Cookie", ck);
                msg = "Passwort geaendert.";
            }
        } else if (!strcmp(act, "remove")) {
            if (!s_pw_set || !authed) {
                msg = "Bitte zuerst anmelden.";
            } else if (!check_pw(cur)) {
                msg = "Aktuelles Passwort ist falsch.";
            } else {
                clear_password();
                msg = "Passwort entfernt. Die Seite ist wieder offen.";
            }
        }
    }

    bool authed_now = session_valid(req) || (req->method == HTTP_POST && ck[0]);
    char body[1500];
    int n = snprintf(body, sizeof(body), "<p class=msg>%s</p>", msg);

    if (!s_pw_set) {
        n += snprintf(body + n, sizeof(body) - n,
            "<p>Aktuell: <b>kein Passwort</b>. Aenderungen sind nur aus vertrauten Netzen moeglich.</p>");
        if (web_trusted(req)) {
            n += snprintf(body + n, sizeof(body) - n,
                "<form method=post action=security><input type=hidden name=action value=set>"
                "Neues Passwort:<br><input name=p1 type=password><br>"
                "Wiederholen:<br><input name=p2 type=password><br>"
                "<button type=submit>Passwort setzen</button></form>");
        } else {
            n += snprintf(body + n, sizeof(body) - n, "<p>Von hier aus kannst du keins setzen.</p>");
        }
    } else if (authed_now) {
        n += snprintf(body + n, sizeof(body) - n,
            "<p>Passwort ist gesetzt.</p>"
            "<h3>Aendern</h3><form method=post action=security><input type=hidden name=action value=change>"
            "Neues Passwort:<br><input name=p1 type=password><br>"
            "Wiederholen:<br><input name=p2 type=password><br>"
            "<button type=submit>Aendern</button></form>"
            "<h3>Entfernen</h3><form method=post action=security><input type=hidden name=action value=remove>"
            "Aktuelles Passwort:<br><input name=cur type=password><br>"
            "<button type=submit>Passwort entfernen</button></form>");
    } else {
        n += snprintf(body + n, sizeof(body) - n,
            "<p>Passwort ist gesetzt. <a href=\"login\">Anmelden</a></p>");
    }
    snprintf(body + n, sizeof(body) - n,
        "<p><small>Vergessen? Am Geraet die BOOT-Taste 5 Sekunden halten, dann wird das Passwort geloescht.</small></p>");
    return web_send_page(req, "Sicherheit", body);
}

static void reg(httpd_handle_t s, const char *uri, httpd_method_t m, esp_err_t (*h)(httpd_req_t *))
{
    httpd_uri_t d = {.uri = uri, .method = m, .handler = h, .user_ctx = NULL};
    httpd_register_uri_handler(s, &d);
}

static void web_http(httpd_handle_t srv)
{
    reg(srv, "/login", HTTP_GET, login_page);
    reg(srv, "/login", HTTP_POST, login_page);
    reg(srv, "/logout", HTTP_GET, logout_page);
    reg(srv, "/security", HTTP_GET, security_page);
    reg(srv, "/security", HTTP_POST, security_page);
}

/* ---------- Passwort-Reset per BOOT-Taste ---------- */

static void reset_task(void *arg)
{
    int held = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level(CONFIG_WEB_RESET_GPIO) == 0) {
            if (++held == 50) { /* 5 s */
                clear_password();
                ESP_LOGW(TAG, "BOOT-Taste 5 s gehalten: Passwort geloescht");
            }
        } else {
            held = 0;
        }
    }
}

static void web_init(void)
{
    load_password();
    ESP_LOGI(TAG, "Passwort: %s", s_pw_set ? "gesetzt" : "keins (offen)");

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_WEB_RESET_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    xTaskCreate(reset_task, "pwreset", 2048, NULL, 2, NULL);
}

static size_t web_pins(module_pin_t *out, size_t max)
{
    if (max < 1) {
        return 0;
    }
    out[0] = (module_pin_t){CONFIG_WEB_RESET_GPIO, false,
                            "Taste gegen GND (BOOT-Taste): 5 s halten loescht das Passwort"};
    return 1;
}

MODULE_REGISTER(web, .name = "web", .stage = MODULE_STAGE_EARLY, .priority = 1,
                .init = web_init, .http = web_http, .pins = web_pins)
