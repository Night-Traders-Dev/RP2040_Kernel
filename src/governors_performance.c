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
    
    /* Ensure we're always targeting MAX */
    if (target_khz != MAX_KHZ) {
        target_khz = MAX_KHZ;
        dmesg_log("gov:performance target reset to MAX");
    }
    
    /* Non-blocking: ramp one step at a time instead of blocking */
    if (target_khz != current_khz) {
        ramp_step(target_khz);
        dmesg_log("gov:performance ramping to MAX");
    }
    sleep_ms(200);
}

static const Governor g = {
    .name = "performance",
    .init = perf_init,
    .tick = perf_tick,
};

const Governor *governor_performance(void) { return &g; }
