#include "governors.h"
#include <stddef.h>
#include <string.h>
#include "persist.h"

/* Registry of built-in governors */
extern const Governor *governor_ondemand(void);
extern const Governor *governor_schedutil(void);
extern const Governor *governor_performance(void);
extern const Governor *governor_rp2040_perf(void);

static const Governor *registry[8];
static size_t registry_n = 0;
static const Governor *current = NULL;

void governors_init(void)
{
    if (registry_n == 0) {
        registry[registry_n++] = governor_ondemand();
        registry[registry_n++] = governor_schedutil();
        registry[registry_n++] = governor_performance();
            registry[registry_n++] = governor_rp2040_perf();
    }

    if (!current) {
        /* Try to load saved governor from persistent storage */
        char saved[64];
        if (persist_load(saved, sizeof(saved)) == 0) {
            const Governor *g = governors_find_by_name(saved);
            if (g) current = g;
        }

        if (!current) {
            /* Prefer rp2040_perf as the kernel governor if available */
            const Governor *pref = governors_find_by_name("rp2040_perf");
            if (pref) current = pref;
            else current = registry[0];
        }
        if (current && current->init) current->init();
    }
}

const Governor *governors_get_current(void)
{
    return current;
}

void governors_set_current(const Governor *g)
{
    current = g;
    if (current && current->init) current->init();
    /* persist selection so it survives reboot */
    if (current && current->name) persist_save(current->name);
}

size_t governors_count(void)
{
    return registry_n;
}

const Governor *governors_get(size_t i)
{
    if (i >= registry_n) return NULL;
    return registry[i];
}

const Governor *governors_find_by_name(const char *name)
{
    for (size_t i = 0; i < registry_n; ++i) {
        if (registry[i] && registry[i]->name && strcmp(registry[i]->name, name) == 0)
            return registry[i];
    }
    return NULL;
}
