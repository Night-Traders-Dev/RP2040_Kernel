#include <stdio.h>
#include "pico/stdlib.h"
#include "system.h"
#include "dmesg.h"
#include "governors.h"

/* Performance governor: always aim for max frequency */

static void perf_init(void)
{
    target_khz = MAX_KHZ;
}

static void perf_tick(const metrics_agg_t *metrics)
{
    core1_wdt_ping++;
    (void)metrics;
    if (target_khz != current_khz)
        ramp_to(target_khz);
    sleep_ms(200);
}

static const Governor g = {
    .name = "performance",
    .init = perf_init,
    .tick = perf_tick,
};

const Governor *governor_performance(void) { return &g; }
