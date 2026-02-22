#ifndef GOVERNORS_H
#define GOVERNORS_H

#include <stdint.h>
#include <stddef.h>

#include "metrics.h"

typedef struct Governor {
    const char *name;
    void (*init)(void);
    /* Called repeatedly on core1; receives latest aggregated metrics (may be NULL) */
    void (*tick)(const metrics_agg_t *metrics);
    /* Optional: export human-readable stats into provided buffer */
    void (*export_stats)(char *buf, size_t len);
} Governor;

void governors_init(void);
const Governor *governors_get_current(void);
void governors_set_current(const Governor *g);

/* Registry access */
size_t governors_count(void);
const Governor *governors_get(size_t i);
const Governor *governors_find_by_name(const char *name);

/* Built-in governors */
const Governor *governor_ondemand(void);
const Governor *governor_schedutil(void);
const Governor *governor_performance(void);
const Governor *governor_rp2040_perf(void);

#endif
