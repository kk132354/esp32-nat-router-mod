#include <stdbool.h>
#include <stdio.h>

#include "esp_log.h"
#include "module_api.h"

#define MAX_MODULES 24

static const char *TAG = "modules";
static const module_t *s_mods[MAX_MODULES];
static int s_count = 0;
static int s_overflow = 0;

/* Laeuft als Konstruktor VOR app_main: nur in ein statisches Array eintragen,
 * kein malloc, kein Logging. */
void module_register(const module_t *m)
{
    if (s_count < MAX_MODULES) {
        s_mods[s_count++] = m;
    } else {
        s_overflow++;
    }
}

static void sort_modules(void)
{
    static bool sorted = false;
    if (sorted) {
        return;
    }
    sorted = true;
    for (int i = 1; i < s_count; i++) { /* Einfuegesortierung nach priority, dann Name */
        const module_t *m = s_mods[i];
        int j = i - 1;
        while (j >= 0 && s_mods[j]->priority > m->priority) {
            s_mods[j + 1] = s_mods[j];
            j--;
        }
        s_mods[j + 1] = m;
    }
}

void modules_run(module_stage_t stage)
{
    sort_modules();
    if (s_overflow) {
        ESP_LOGE(TAG, "%d Module passen nicht ins Register (MAX_MODULES=%d)", s_overflow, MAX_MODULES);
    }
    for (int i = 0; i < s_count; i++) {
        if (s_mods[i]->stage == stage && s_mods[i]->init) {
            ESP_LOGI(TAG, "starte Modul \"%s\" (Stufe %d)", s_mods[i]->name, (int)stage);
            s_mods[i]->init();
        }
    }
}

size_t modules_status(char *buf, size_t len)
{
    size_t n = 0;
    if (len == 0) {
        return 0;
    }
    buf[0] = '\0';
    sort_modules();
    for (int i = 0; i < s_count; i++) {
        if (!s_mods[i]->status || n + 1 >= len) {
            continue;
        }
        int w = s_mods[i]->status(buf + n, len - n);
        if (w > 0) {
            n += ((size_t)w < len - n) ? (size_t)w : (len - n - 1);
        }
    }
    return n;
}
