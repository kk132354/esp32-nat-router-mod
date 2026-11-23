/* Startseite "/": Status von main.c und aller Module. */
#include <stdio.h>

#include "esp_http_server.h"
#include "module_api.h"
#include "web.h"

static esp_err_t home_page(httpd_req_t *req)
{
    char core[800], mods[600], body[1700];
    modules_core_status(core, sizeof(core));
    modules_status(mods, sizeof(mods));
    snprintf(body, sizeof(body),
        "<pre>%s\n%s</pre>"
        "<script>setTimeout(function(){location.reload()},5000)</script>", core, mods);
    return web_send_page(req, "Status", body);
}

static void home_http(httpd_handle_t srv)
{
    httpd_uri_t d = {.uri = "/", .method = HTTP_GET, .handler = home_page, .user_ctx = NULL};
    httpd_register_uri_handler(srv, &d);
}

MODULE_REGISTER(home, .name = "home", .stage = MODULE_STAGE_EARLY, .priority = 2, .http = home_http)
