/*
 * Gemeinsame Web-Hilfen fuer alle Module (Seitenlayout, Anmeldung, Formulare).
 * Eine neue Webseite braucht nur:  web_send_page(req, "Titel", "<p>HTML</p>");
 * Links in Seiten IMMER relativ schreiben ("fan", nicht "/fan"), sonst bricht die Gegenstellen-Ansicht (/peer/).
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "esp_http_server.h"

/* Seite mit Kopf, Menue und Fusszeile senden. */
esp_err_t web_send_page(httpd_req_t *req, const char *title, const char *body_html);

/* Darf diese Anfrage etwas AENDERN? (Passwort gesetzt: nur mit Anmeldung; sonst nur aus vertrauten Netzen) */
bool web_can_control(httpd_req_t *req);

/* Kommt die Anfrage aus einem vertrauten Netz (SoftAP, Tunnel, evtl. Heimnetz)? */
bool web_trusted(httpd_req_t *req);

bool web_password_set(void);

/* POST-Koerper lesen (malloc, mit free() freigeben; NULL bei Fehler/zu gross). */
char *web_read_body(httpd_req_t *req);

/* Wert aus "a=1&b=2" holen, URL-dekodiert. */
bool web_form_get(const char *body, const char *key, char *out, size_t len);
