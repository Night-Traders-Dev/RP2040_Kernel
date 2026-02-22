#include <stdio.h>
#include "pico/stdlib.h"
#include "system.h"
#include "dmesg.h"
#include "governors.h"

/* Schedutil-style governor: try to follow a 'utilization' estimate.
   Here we simulate utilization by sampling temperature variance as a placeholder. */

static void sch_init(void) { }

static void sch_tick(const metrics_agg_t *metrics)
{
    core1_wdt_ping++;
    float temp = read_onboard_temperature();

    /* Prefer app-reported utilization if available */
    int util;
    if (metrics && metrics->count > 0) {
        util = (int)metrics->avg_intensity;
    } else {
        /* fake util: mapped from temperature (just for testing) */
        util = (int) ( (temp - 30.0f) );
    }
    if (util < 0) util = 0;
    if (util > 100) util = 100;

    uint32_t target = MIN_KHZ + (uint32_t)((MAX_KHZ - MIN_KHZ) * util / 100);
    if (target > MAX_KHZ) target = MAX_KHZ;
    if (target < MIN_KHZ) target = MIN_KHZ;

    if (target_khz != target) {
        target_khz = target;
        dmesg_log("gov:schedutil adjusted target");
    }

    if (target_khz != current_khz)
        ramp_to(target_khz);

    (void)metrics;
    sleep_ms(60);
}

static const Governor g = {
    .name = "schedutil",
    .init = sch_init,
    .tick = sch_tick,
};

const Governor *governor_schedutil(void) { return &g; }
