/*
 * Gegenstelle: /peer/... leitet die Anfrage an das andere Geraet durch den Tunnel weiter.
 * So steuerst du vom DevKit aus den C3 (und umgekehrt) ueber eine einzige Adresse.
 * Anmeldung und Rechte gelten weiterhin auf dem Zielgeraet.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "module_api.h"
#include "web.h"

static const char *TAG = "peer";

typedef struct {
    char ctype[64];
    char setcookie[200];
} hdrs_t;

static esp_err_t on_http_event(esp_http_client_event_t *e)
{
    hdrs_t *h = (hdrs_t *)e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER && h) {
        if (strcasecmp(e->header_key, "Content-Type") == 0) {
            strlcpy(h->ctype, e->header_value, sizeof(h->ctype));
        } else if (strcasecmp(e->header_key, "Set-Cookie") == 0 && !h->setcookie[0]) {
            strlcpy(h->setcookie, e->header_value, sizeof(h->setcookie));
        }
    }
    return ESP_OK;
}

static esp_err_t peer_handler(httpd_req_t *req)
{
    char tmp[8];
    if (httpd_req_get_hdr_value_str(req, "X-Proxied", tmp, sizeof(tmp)) == ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Keine Weiterleitung ueber zwei Geraete");
    }
    if (strcmp(req->uri, "/peer") == 0) { /* damit relative Links stimmen */
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/peer/");
        return httpd_resp_send(req, NULL, 0);
    }

    const char *path = req->uri + 5; /* hinter "/peer" */
    char url[220];
    snprintf(url, sizeof(url), "http://%s%s", CONFIG_WEB_PEER_IP, path);

    hdrs_t h = {{0}, {0}};
    bool post = (req->method == HTTP_POST);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = (post && req->content_len > 600) ? 30000 : 8000,
        .event_handler = on_http_event,
        .user_data = &h,
        .disable_auto_redirect = true,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Proxy-Fehler");
    }

    char cookie[320];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) == ESP_OK) {
        esp_http_client_set_header(c, "Cookie", cookie);
    }
    esp_http_client_set_header(c, "X-Proxied", "1");
    esp_http_client_set_header(c, "X-Peer-Trusted", web_trusted(req) ? "1" : "0");

    char xo[4];
    if (httpd_req_get_hdr_value_str(req, "X-OTA", xo, sizeof(xo)) == ESP_OK) {
        esp_http_client_set_header(c, "X-OTA", xo);
    }

    /* Kleine Formulare (Passwort, Luefter ...) liegen im Speicher, grosse Uploads
     * (Firmware) werden Stueck fuer Stueck durchgereicht. */
    char body[600];
    int blen = 0;
    const size_t total = post ? req->content_len : 0;
    const bool stream = total > sizeof(body);
    if (post && !stream) {
        while (blen < (int)total) {
            int r = httpd_req_recv(req, body + blen, total - blen);
            if (r <= 0) {
                esp_http_client_cleanup(c);
                return ESP_FAIL;
            }
            blen += r;
        }
        esp_http_client_set_header(c, "Content-Type", "application/x-www-form-urlencoded");
    }
    if (stream) {
        if (total > 4 * 1024 * 1024) {
            esp_http_client_cleanup(c);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Zu gross");
        }
        esp_http_client_set_header(c, "Content-Type", "application/octet-stream");
    }

    esp_err_t err = esp_http_client_open(c, stream ? (int)total : blen);
    if (err == ESP_OK && blen > 0 && esp_http_client_write(c, body, blen) < 0) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && stream) {
        char *sb = malloc(1024);
        if (!sb) {
            err = ESP_ERR_NO_MEM;
        }
        size_t sent = 0;
        int idle = 0;
        while (err == ESP_OK && sent < total) {
            size_t want = total - sent > 1024 ? 1024 : total - sent;
            int r = httpd_req_recv(req, sb, want);
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++idle > 6) {
                    err = ESP_ERR_TIMEOUT;
                }
                continue;
            }
            if (r <= 0) {
                err = ESP_FAIL;
                break;
            }
            idle = 0;
            for (int w = 0; w < r && err == ESP_OK;) {
                int k = esp_http_client_write(c, sb + w, r - w);
                if (k <= 0) {
                    err = ESP_FAIL;
                } else {
                    w += k;
                }
            }
            sent += (size_t)r;
        }
        free(sb);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Gegenstelle nicht erreichbar: %s", esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Gegenstelle nicht erreichbar");
    }
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);

    char buf[512];
    int n = esp_http_client_read(c, buf, sizeof(buf));
    if (n < 0) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Keine Antwort der Gegenstelle");
    }
    if (status == 404) {
        httpd_resp_set_status(req, "404 Not Found");
    } else if (status != 200) {
        httpd_resp_set_status(req, "502 Bad Gateway");
    }
    httpd_resp_set_type(req, h.ctype[0] ? h.ctype : "text/html");
    if (h.setcookie[0]) {
        httpd_resp_set_hdr(req, "Set-Cookie", h.setcookie);
    }
    while (n > 0) {
        httpd_resp_send_chunk(req, buf, n);
        n = esp_http_client_read(c, buf, sizeof(buf));
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static void peer_http(httpd_handle_t srv)
{
    httpd_uri_t g = {.uri = "/peer*", .method = HTTP_GET, .handler = peer_handler, .user_ctx = NULL};
    httpd_uri_t p = {.uri = "/peer*", .method = HTTP_POST, .handler = peer_handler, .user_ctx = NULL};
    httpd_register_uri_handler(srv, &g);
    httpd_register_uri_handler(srv, &p);
}

MODULE_REGISTER(peer, .name = "peer", .stage = MODULE_STAGE_EARLY, .priority = 3, .http = peer_http)
