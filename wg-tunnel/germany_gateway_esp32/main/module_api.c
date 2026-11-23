#include <stdbool.h>
#include <stdio.h>
#include <string.h>

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

void modules_register_http(httpd_handle_t srv)
{
    sort_modules();
    for (int i = 0; i < s_count; i++) {
        if (s_mods[i]->http) {
            s_mods[i]->http(srv);
        }
    }
}

size_t modules_nav_html(char *buf, size_t len)
{
    size_t n = 0;
    if (len == 0) {
        return 0;
    }
    buf[0] = '\0';
    sort_modules();
    for (int i = 0; i < s_count; i++) {
        if (!s_mods[i]->menu_title || !s_mods[i]->menu_path) {
            continue;
        }
        int w = snprintf(buf + n, len - n, "<a href=\"%s\">%s</a> ", s_mods[i]->menu_path, s_mods[i]->menu_title);
        if (w < 0 || (size_t)w >= len - n) {
            break;
        }
        n += (size_t)w;
    }
    return n;
}

static int (*s_core_status)(char *buf, size_t len);

void modules_set_core_status(int (*fn)(char *buf, size_t len))
{
    s_core_status = fn;
}

size_t modules_core_status(char *buf, size_t len)
{
    if (len == 0) {
        return 0;
    }
    buf[0] = '\0';
    if (!s_core_status) {
        return 0;
    }
    int w = s_core_status(buf, len);
    if (w <= 0) {
        return 0;
    }
    return ((size_t)w < len) ? (size_t)w : len - 1;
}

static volatile bool s_healthy = false;
void modules_report_healthy(void) { s_healthy = true; }
bool modules_is_healthy(void) { return s_healthy; }

/* ---------- Pinbelegung ---------- */

#define MAX_PINS 8

static const char *pin_warning(const module_pin_t *p)
{
#ifdef CONFIG_IDF_TARGET_ESP32C3
    if (p->gpio >= 12 && p->gpio <= 17) return "Flash-Pin, nicht benutzen!";
    if (p->gpio == 18 || p->gpio == 19) return "USB-Pin (Programmierung/Konsole)";
    if (p->output && (p->gpio == 2 || p->gpio == 8 || p->gpio == 9)) return "Boot-Pin: Schaltung kann den Start stoeren";
#else
    if (p->gpio >= 6 && p->gpio <= 11) return "Flash-Pin, nicht benutzen!";
    if (p->output && p->gpio >= 34 && p->gpio <= 39) return "nur Eingang, kann nicht schalten!";
    if (p->output && (p->gpio == 0 || p->gpio == 2 || p->gpio == 12 || p->gpio == 15)) return "Boot-Pin: Schaltung kann den Start stoeren";
#endif
    return NULL;
}

size_t modules_pins_html(char *buf, size_t len, const char *only)
{
    size_t n = 0;
    if (len == 0) {
        return 0;
    }
    buf[0] = '\0';
    sort_modules();
#define ADD(...)                                                        \
    do {                                                                \
        if (n + 1 < len) {                                              \
            int w_ = snprintf(buf + n, len - n, __VA_ARGS__);           \
            if (w_ > 0) n += ((size_t)w_ < len - n) ? (size_t)w_ : len - n - 1; \
        }                                                               \
    } while (0)

    if (!only) { /* Doppelt belegte Pins finden */
        for (int i = 0; i < s_count; i++) {
            module_pin_t a[MAX_PINS];
            size_t na = s_mods[i]->pins ? s_mods[i]->pins(a, MAX_PINS) : 0;
            for (size_t x = 0; x < na; x++) {
                if (a[x].gpio < 0) continue;
                for (int j = i; j < s_count; j++) {
                    module_pin_t b[MAX_PINS];
                    size_t nb = s_mods[j]->pins ? s_mods[j]->pins(b, MAX_PINS) : 0;
                    for (size_t y = (j == i ? x + 1 : 0); y < nb; y++) {
                        if (b[y].gpio == a[x].gpio) {
                            ADD("<p class=msg><b>ACHTUNG:</b> GPIO %d ist doppelt belegt (%s und %s)!</p>",
                                a[x].gpio, s_mods[i]->name, s_mods[j]->name);
                        }
                    }
                }
            }
        }
    }
    for (int i = 0; i < s_count; i++) {
        if (!s_mods[i]->pins || (only && strcmp(only, s_mods[i]->name) != 0)) {
            continue;
        }
        module_pin_t p[MAX_PINS];
        size_t np = s_mods[i]->pins(p, MAX_PINS);
        if (np == 0) {
            continue;
        }
        ADD("<h3>Modul %s</h3><ul>", s_mods[i]->name);
        for (size_t k = 0; k < np; k++) {
            const char *warn = p[k].gpio >= 0 ? pin_warning(&p[k]) : NULL;
            if (p[k].gpio >= 0) {
                ADD("<li><b>GPIO %d</b>: %s%s%s%s</li>", p[k].gpio, p[k].role,
                    warn ? " <span class=msg>(" : "", warn ? warn : "", warn ? ")</span>" : "");
            } else {
                ADD("<li>%s</li>", p[k].role);
            }
        }
        ADD("</ul>");
    }
#undef ADD
    return n;
}
