/*
 * Seite "Pins": zeigt die Pinbelegung ALLER Module auf einen Blick
 * und warnt vor doppelt belegten oder ungeeigneten Pins.
 * Ein Modul meldet seine Pins mit  .pins = meine_funktion  (siehe mod_fan.c).
 */
#include <stdio.h>
#include <stdlib.h>

#include "esp_http_server.h"

#include "module_api.h"
#include "web.h"

static esp_err_t pins_page(httpd_req_t *req)
{
    const size_t SZ = 4096;
    char *b = malloc(SZ);
    if (!b) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Kein Speicher");
    }
    if (modules_pins_html(b, SZ, NULL) == 0) {
        snprintf(b, SZ, "<p>Kein Modul meldet Pins.</p>");
    }
    esp_err_t r = web_send_page(req, "Pins", b);
    free(b);
    return r;
}

static void pins_http(httpd_handle_t srv)
{
    httpd_uri_t u = {.uri = "/pins", .method = HTTP_GET, .handler = pins_page};
    httpd_register_uri_handler(srv, &u);
}

MODULE_REGISTER(pins, .name = "pins", .stage = MODULE_STAGE_EARLY, .priority = 4,
                .http = pins_http, .menu_title = "Pins", .menu_path = "pins")
