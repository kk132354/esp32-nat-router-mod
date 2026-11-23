/*
 * Mini-Modulsystem: Jede Datei  main/mod_<name>.c  meldet sich selbst an
 * (MODULE_REGISTER) und wird von main.c zum richtigen Zeitpunkt gestartet.
 * main.c muss fuer ein neues Modul NICHT geaendert werden.
 *
 * Ein Modul kann: beim Start etwas tun (.init), Statuszeilen liefern (.status),
 * eigene Webseiten anmelden (.http) und einen Menuepunkt bekommen
 * (.menu_title + .menu_path, relativer Pfad ohne fuehrenden "/", z. B. "fan").
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>

#include "esp_http_server.h"

typedef enum {
    MODULE_STAGE_EARLY,    /* direkt nach dem Start, vor dem WLAN        */
    MODULE_STAGE_NETWORK,  /* WLAN verbunden und Uhrzeit synchronisiert  */
    MODULE_STAGE_TUNNEL,   /* WireGuard-Tunnel ist eingerichtet          */
    MODULE_STAGE_COUNT
} module_stage_t;

/* Eine Zeile der Pinbelegung eines Moduls.
 * gpio >= 0 : echter Pin. gpio = -1 : nur Verdrahtungshinweis (Stromversorgung, Bauteil ...).
 * output    : true, wenn das Modul den Pin als Ausgang schaltet. */
typedef struct {
    int gpio;
    bool output;
    const char *role;
} module_pin_t;

typedef struct {
    const char *name;
    module_stage_t stage;
    int priority;                          /* kleiner = frueher (Start und Menuereihenfolge) */
    void (*init)(void);                    /* wird in der Stufe einmal aufgerufen            */
    int (*status)(char *buf, size_t len);  /* optional: Textzeilen fuer die Startseite       */
    void (*http)(httpd_handle_t srv);      /* optional: eigene Webseiten anmelden            */
    const char *menu_title;                /* optional: Text im Menue                        */
    const char *menu_path;                 /* optional: relativer Link, z. B. "fan"          */
    size_t (*pins)(module_pin_t *out, size_t max); /* optional: Pinbelegung (max. 8 Zeilen)  */
} module_t;

void module_register(const module_t *m);
void modules_run(module_stage_t stage);
size_t modules_status(char *buf, size_t len);
void modules_register_http(httpd_handle_t srv);
size_t modules_nav_html(char *buf, size_t len);
/* HTML-Liste der Pinbelegung: only = Modulname oder NULL fuer alle (dann mit Konfliktwarnung). */
size_t modules_pins_html(char *buf, size_t len, const char *only);

/* main.c meldet hier seine Statuszeilen an (Netz, Tunnel ...). */
void modules_set_core_status(int (*fn)(char *buf, size_t len));
size_t modules_core_status(char *buf, size_t len);

/* "Das Geraet ist gesund" (Tunnel steht). Wird von main.c gemeldet; das OTA-Modul
 * bestaetigt damit nach einem Update die neue Firmware, sonst gibt es ein Rollback. */
void modules_report_healthy(void);
bool modules_is_healthy(void);

#define MODULE_REGISTER(ID, ...)                                           \
    static const module_t module_def_##ID = {__VA_ARGS__};                 \
    static void __attribute__((constructor)) module_ctor_##ID(void)        \
    {                                                                      \
        module_register(&module_def_##ID);                                 \
    }
