/*
 * Mini-Modulsystem: Jede Datei  main/mod_<name>.c  meldet sich selbst an
 * (MODULE_REGISTER) und wird von main.c zum richtigen Zeitpunkt gestartet.
 * main.c muss fuer ein neues Modul NICHT geaendert werden.
 */
#pragma once
#include <stddef.h>

typedef enum {
    MODULE_STAGE_EARLY,    /* direkt nach dem Start, vor dem WLAN        */
    MODULE_STAGE_NETWORK,  /* WLAN verbunden und Uhrzeit synchronisiert  */
    MODULE_STAGE_TUNNEL,   /* WireGuard-Tunnel ist eingerichtet          */
    MODULE_STAGE_COUNT
} module_stage_t;

typedef struct {
    const char *name;
    module_stage_t stage;
    int priority;                          /* kleiner = frueher (innerhalb einer Stufe) */
    void (*init)(void);                    /* wird in der Stufe einmal aufgerufen       */
    int (*status)(char *buf, size_t len);  /* optional: Textzeilen fuer die Statusseite */
} module_t;

void module_register(const module_t *m);
void modules_run(module_stage_t stage);
size_t modules_status(char *buf, size_t len);

/* Beispiel: MODULE_REGISTER(fan, .name = "fan", .stage = MODULE_STAGE_EARLY,
 *                           .priority = 10, .init = fan_init, .status = fan_status) */
#define MODULE_REGISTER(ID, ...)                                           \
    static const module_t module_def_##ID = {__VA_ARGS__};                 \
    static void __attribute__((constructor)) module_ctor_##ID(void)        \
    {                                                                      \
        module_register(&module_def_##ID);                                 \
    }
