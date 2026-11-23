/*
 * Firmware-Update ueber die Webseite (OTA) mit automatischem Zurueckrollen.
 *
 *  - /ota        : Seite mit Version, Status und Upload-Formular (Upload nur mit Passwort!)
 *  - POST /ota   : nimmt die Firmware-Datei (build/<Projekt>.bin) entgegen, schreibt sie in den
 *                  freien Programmplatz (ota_0 / ota_1), prueft sie und startet neu.
 *  - Nach dem Neustart gilt die neue Firmware nur als "zur Probe". Sobald der Tunnel steht
 *    (main.c ruft modules_report_healthy()), wird sie bestaetigt. Passiert das nicht innerhalb
 *    von OTA_VERIFY_S Sekunden, oder startet das Geraet vorher neu/stuerzt ab, startet der
 *    Bootloader wieder die ALTE Firmware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "module_api.h"
#include "web.h"

#ifndef CONFIG_OTA_VERIFY_S
#define CONFIG_OTA_VERIFY_S 300
#endif

#define CHUNK 2048
#define HEAD_LEN 512 /* so viel lesen wir zuerst, um die Datei zu pruefen */

static const char *TAG = "ota";
static volatile bool s_busy = false;
static bool s_pending = false;

/* ---------- Hilfen ---------- */

static void reply(httpd_req_t *req, const char *status, const char *text)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

/* Rest des Uploads verwerfen, damit der Browser unsere Fehlermeldung wirklich sieht. */
static void drain(httpd_req_t *req, size_t remaining)
{
    if (remaining > 0x300000) {
        return;
    }
    char *b = malloc(512);
    if (!b) {
        return;
    }
    int idle = 0;
    while (remaining > 0 && idle < 5) {
        int r = httpd_req_recv(req, b, remaining > 512 ? 512 : remaining);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            idle++;
            continue;
        }
        if (r <= 0) {
            break;
        }
        idle = 0;
        remaining -= (size_t)r;
    }
    free(b);
}

static void fail(httpd_req_t *req, const char *status, const char *text, size_t remaining)
{
    ESP_LOGW(TAG, "Update abgelehnt: %s", text);
    drain(req, remaining);
    reply(req, status, text);
}

/* genau n Bytes lesen */
static bool recv_full(httpd_req_t *req, char *buf, size_t n)
{
    size_t got = 0;
    int idle = 0;
    while (got < n) {
        int r = httpd_req_recv(req, buf + got, n - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++idle > 6) {
                return false;
            }
            continue;
        }
        if (r <= 0) {
            return false;
        }
        idle = 0;
        got += (size_t)r;
    }
    return true;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500)); /* Antwort an den Browser noch rausgeben */
    esp_restart();
}

static void schedule_restart(void)
{
    if (xTaskCreate(restart_task, "ota_rst", 2048, NULL, 5, NULL) != pdPASS) {
        esp_restart();
    }
}

static bool may_update(httpd_req_t *req)
{
    /* Firmware-Upload = Code ausfuehren. Deshalb NUR mit gesetztem Passwort und gueltiger Anmeldung. */
    return web_password_set() && web_can_control(req);
}

/* ---------- Upload ---------- */

static esp_err_t ota_post(httpd_req_t *req)
{
    size_t total = req->content_len;
    char tmp[4];
    /* Schutz gegen fremde Webseiten (CSRF): Ein Browser darf diesen Zusatz-Header
     * nur von UNSERER Seite aus senden. */
    if (httpd_req_get_hdr_value_str(req, "X-OTA", tmp, sizeof(tmp)) != ESP_OK) {
        fail(req, "400 Bad Request", "Fehlender X-OTA-Header", total);
        return ESP_OK;
    }
    if (!may_update(req)) {
        fail(req, "403 Forbidden", web_password_set() ? "Nicht angemeldet." : "Bitte zuerst unter Sicherheit ein Passwort setzen.", total);
        return ESP_OK;
    }
    if (s_busy) {
        fail(req, "409 Conflict", "Es laeuft schon ein Update.", total);
        return ESP_OK;
    }
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *upd = esp_ota_get_next_update_partition(NULL);
    if (!run || !upd) {
        fail(req, "500 Internal Server Error", "Keine Update-Partition gefunden (Partitionstabelle?).", total);
        return ESP_OK;
    }
    if (total < 100000 || total > upd->size) {
        fail(req, "400 Bad Request", "Dateigroesse passt nicht (zu klein oder groesser als der Programmplatz).", total);
        return ESP_OK;
    }

    s_busy = true;
    char *buf = malloc(CHUNK);
    if (!buf) {
        s_busy = false;
        fail(req, "500 Internal Server Error", "Kein Speicher.", total);
        return ESP_OK;
    }

    /* 1. Dateikopf pruefen: ESP-Firmware? Richtiges Projekt? */
    if (!recv_full(req, buf, HEAD_LEN)) {
        free(buf);
        s_busy = false;
        return ESP_FAIL;
    }
    size_t remaining = total - HEAD_LEN;
    const size_t desc_off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    esp_app_desc_t nd, rd;
    memcpy(&nd, buf + desc_off, sizeof(nd));
    if ((uint8_t)buf[0] != 0xE9 || nd.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        free(buf);
        s_busy = false;
        fail(req, "400 Bad Request",
             "Das ist keine Firmware-Datei. Nimm build/<Projektname>.bin, NICHT merged-binary.bin.", remaining);
        return ESP_OK;
    }
    if (esp_ota_get_partition_description(run, &rd) != ESP_OK || strncmp(nd.project_name, rd.project_name, sizeof(nd.project_name)) != 0) {
        free(buf);
        s_busy = false;
        fail(req, "400 Bad Request", "Falsches Projekt: Diese Firmware gehoert zu einem anderen Geraet.", remaining);
        return ESP_OK;
    }

    /* 2. Schreiben */
    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(upd, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) {
        free(buf);
        s_busy = false;
        fail(req, "500 Internal Server Error", "Update konnte nicht gestartet werden.", remaining);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Update startet: %u Bytes nach %s", (unsigned)total, upd->label);
    err = esp_ota_write(h, buf, HEAD_LEN);
    int idle = 0;
    while (err == ESP_OK && remaining > 0) {
        size_t want = remaining > CHUNK ? CHUNK : remaining;
        int r = httpd_req_recv(req, buf, want);
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
        err = esp_ota_write(h, buf, (size_t)r);
        remaining -= (size_t)r;
    }
    free(buf);
    if (err != ESP_OK) {
        esp_ota_abort(h);
        s_busy = false;
        ESP_LOGW(TAG, "Schreiben abgebrochen: %s", esp_err_to_name(err));
        if (err != ESP_FAIL && err != ESP_ERR_TIMEOUT) {
            reply(req, "500 Internal Server Error", "Schreibfehler. Altes Programm bleibt aktiv.");
            return ESP_OK;
        }
        return ESP_FAIL; /* Verbindung weg */
    }

    /* 3. Pruefsumme kontrollieren und erst dann umschalten */
    err = esp_ota_end(h);
    if (err != ESP_OK) {
        s_busy = false;
        reply(req, "400 Bad Request", "Pruefung fehlgeschlagen (Datei kaputt oder falscher Chip). Altes Programm bleibt aktiv.");
        return ESP_OK;
    }
    err = esp_ota_set_boot_partition(upd);
    if (err != ESP_OK) {
        s_busy = false;
        reply(req, "500 Internal Server Error", "Umschalten fehlgeschlagen. Altes Programm bleibt aktiv.");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Update ok, Neustart in 1,5 s");
    reply(req, "200 OK", "OK");
    schedule_restart(); /* s_busy bleibt true, es startet sowieso neu */
    return ESP_OK;
}

/* ---------- Zurueck zur vorherigen Version (von Hand) ---------- */

static esp_err_t rollback_post(httpd_req_t *req)
{
    char tmp[4];
    if (httpd_req_get_hdr_value_str(req, "X-OTA", tmp, sizeof(tmp)) != ESP_OK) {
        reply(req, "400 Bad Request", "Fehlender X-OTA-Header");
        return ESP_OK;
    }
    if (!may_update(req)) {
        reply(req, "403 Forbidden", "Nur mit Passwort und Anmeldung.");
        return ESP_OK;
    }
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t d;
    if (!other || esp_ota_get_partition_description(other, &d) != ESP_OK) {
        reply(req, "400 Bad Request", "Es gibt keine vorherige Version.");
        return ESP_OK;
    }
    if (esp_ota_set_boot_partition(other) != ESP_OK) {
        reply(req, "500 Internal Server Error", "Umschalten fehlgeschlagen.");
        return ESP_OK;
    }
    reply(req, "200 OK", "OK");
    schedule_restart();
    return ESP_OK;
}

/* ---------- Seite ---------- */

static const char *state_text(esp_ota_img_states_t s, bool ok)
{
    if (!ok) {
        return "Werkseinspielung (per Kabel geflasht)";
    }
    switch (s) {
    case ESP_OTA_IMG_NEW: return "neu";
    case ESP_OTA_IMG_PENDING_VERIFY: return "wird geprueft (Rollback moeglich)";
    case ESP_OTA_IMG_VALID: return "bestaetigt";
    case ESP_OTA_IMG_INVALID: return "ungueltig";
    case ESP_OTA_IMG_ABORTED: return "abgebrochen";
    default: return "unbekannt";
    }
}

static const char *PAGE_JS =
    "<script>"
    "function send(url,file,done){var x=new XMLHttpRequest();x.open('POST',url);"
    "x.setRequestHeader('X-OTA','1');"
    "x.upload.onprogress=function(e){if(e.lengthComputable)document.getElementById('st').textContent="
    "'Hochladen: '+Math.round(e.loaded*100/e.total)+' %';};"
    "x.onload=function(){done(x.status,x.responseText);};"
    "x.onerror=function(){done(0,'Verbindung abgebrochen.');};"
    "x.send(file);}"
    "function waitBack(){var n=0;var t=setInterval(function(){n++;"
    "document.getElementById('st').textContent='Neustart ... '+n*3+' s';"
    "fetch('ota',{cache:'no-store'}).then(function(r){if(r.ok){clearInterval(t);location.reload();}}).catch(function(){});"
    "if(n>150){clearInterval(t);document.getElementById('st').textContent='Geraet meldet sich nicht. Pruefe es.';}},3000);}"
    "function upl(){var f=document.getElementById('f').files[0];if(!f){alert('Datei waehlen');return;}"
    "if(!confirm('Firmware '+f.name+' ('+Math.round(f.size/1024)+' KB) jetzt aufspielen?'))return;"
    "document.getElementById('go').disabled=true;"
    "send('ota',f,function(s,t){if(s==200){document.getElementById('st').textContent='Gespeichert, Neustart ...';"
    "setTimeout(waitBack,4000);}else{document.getElementById('st').textContent='Fehler: '+t;"
    "document.getElementById('go').disabled=false;}});}"
    "function rb(){if(!confirm('Zur vorherigen Version zurueck?'))return;"
    "send('otarollback',null,function(s,t){if(s==200){document.getElementById('st').textContent='Neustart ...';setTimeout(waitBack,4000);}"
    "else document.getElementById('st').textContent='Fehler: '+t;});}"
    "</script>";

static esp_err_t ota_get(httpd_req_t *req)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t d = {0}, od;
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    bool have_state = run && esp_ota_get_state_partition(run, &st) == ESP_OK;
    bool have_desc = run && esp_ota_get_partition_description(run, &d) == ESP_OK;
    bool have_other = other && esp_ota_get_partition_description(other, &od) == ESP_OK;
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();

    const size_t BODY_SZ = 4096;
    char *body = malloc(BODY_SZ);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Kein Speicher");
    }
    int n = snprintf(body, BODY_SZ,
        "<p>Programmplatz: <b>%s</b><br>Projekt: %s<br>Version: %s<br>Gebaut: %s %s<br>Status: %s</p>",
        run ? run->label : "?", have_desc ? d.project_name : "?", have_desc ? d.version : "?",
        have_desc ? d.date : "", have_desc ? d.time : "", state_text(st, have_state));
    if (bad) {
        n += snprintf(body + n, BODY_SZ - n,
            "<p class=msg>Ein Update auf Platz <b>%s</b> war nicht erfolgreich und wurde zurueckgerollt.</p>", bad->label);
    }
    if (have_other) {
        n += snprintf(body + n, BODY_SZ - n, "<p>Vorherige Version (Platz %s): %s, gebaut %s %s</p>",
                      other->label, od.version, od.date, od.time);
    }
    if (may_update(req)) {
        n += snprintf(body + n, BODY_SZ - n,
            "<hr><p>Firmware-Datei waehlen (<b>build/%s.bin</b>):</p>"
            "<input type=file id=f accept='.bin'><br>"
            "<button id=go onclick='upl()'>Hochladen und installieren</button>"
            "%s"
            "<p id=st></p>"
            "<p>Nach dem Neustart wird die neue Version nur bestaetigt, wenn der Tunnel steht (max. %d s). "
            "Sonst startet automatisch wieder die alte Version.</p>%s",
            have_desc ? d.project_name : "NAME",
            have_other ? "<br><button onclick='rb()'>Zurueck zur vorherigen Version</button>" : "",
            CONFIG_OTA_VERIFY_S, PAGE_JS);
    } else if (!web_password_set()) {
        n += snprintf(body + n, BODY_SZ - n,
            "<hr><p>Zum Aufspielen von Firmware zuerst unter <a href=\"security\">Sicherheit</a> ein Passwort setzen.</p>");
    } else {
        n += snprintf(body + n, BODY_SZ - n,
            "<hr><p>Zum Aufspielen bitte <a href=\"login\">anmelden</a>.</p>");
    }
    (void)n;
    esp_err_t res = web_send_page(req, "Firmware", body);
    free(body);
    return res;
}

/* ---------- Bestaetigen oder Zurueckrollen ---------- */

static void verify_task(void *arg)
{
    for (int i = 0; i < CONFIG_OTA_VERIFY_S; i++) {
        if (modules_is_healthy()) {
            esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "Neue Firmware laeuft (Tunnel steht): als gueltig bestaetigt (%s)", esp_err_to_name(e));
            s_pending = false;
            vTaskDelete(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGE(TAG, "Neue Firmware wurde nicht gesund (kein Tunnel in %d s): ROLLBACK", CONFIG_OTA_VERIFY_S);
    esp_ota_mark_app_invalid_rollback_and_reboot();
    esp_restart(); /* nur falls obiges fehlschlaegt */
}

static void ota_init(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending = true;
        ESP_LOGW(TAG, "Neue Firmware auf Probe. Bestaetigung, sobald der Tunnel steht (max. %d s).", CONFIG_OTA_VERIFY_S);
        xTaskCreate(verify_task, "ota_verify", 3072, NULL, 5, NULL);
    }
}

static int ota_status(char *buf, size_t len)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_app_desc_t d;
    if (!run || esp_ota_get_partition_description(run, &d) != ESP_OK) {
        return 0;
    }
    return snprintf(buf, len, "Firmware:       %s (%s)%s\n", d.version, run->label, s_pending ? " AUF PROBE" : "");
}

static void ota_http(httpd_handle_t srv)
{
    httpd_uri_t g = {.uri = "/ota", .method = HTTP_GET, .handler = ota_get};
    httpd_uri_t p = {.uri = "/ota", .method = HTTP_POST, .handler = ota_post};
    httpd_uri_t r = {.uri = "/otarollback", .method = HTTP_POST, .handler = rollback_post};
    httpd_register_uri_handler(srv, &g);
    httpd_register_uri_handler(srv, &p);
    httpd_register_uri_handler(srv, &r);
}

MODULE_REGISTER(ota, .name = "ota", .stage = MODULE_STAGE_EARLY, .priority = 5, .init = ota_init,
                .status = ota_status, .http = ota_http, .menu_title = "Firmware", .menu_path = "ota")
