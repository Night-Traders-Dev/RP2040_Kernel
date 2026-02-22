#include <stdio.h>
#include "pico/stdlib.h"
#include "system.h"
#include "dmesg.h"
#include "governors.h"

/* On-demand governor: ramp up aggressively when activity seen, back off slowly.
   For testing we use temperature as proxy for activity. */

static void ond_init(void) { }

static void ond_tick(const metrics_agg_t *metrics)
{
    core1_wdt_ping++;
    float temp = read_onboard_temperature();

    /* If apps report activity, be more aggressive */
    if (metrics && metrics->count > 0 && metrics->avg_intensity > 70.0) {
        if (target_khz < MAX_KHZ) target_khz += 30000;
        if (target_khz > MAX_KHZ) target_khz = MAX_KHZ;
        dmesg_log("gov:ondemand ramp up (metrics)");
    } else if (temp < 50.0f && target_khz < MAX_KHZ) {
        target_khz += 20000; /* ramp quickly up */
        if (target_khz > MAX_KHZ) target_khz = MAX_KHZ;
        dmesg_log("gov:ondemand ramp up");
    } else if (temp > 65.0f && target_khz > 125000) {
        target_khz -= 10000; /* back off slower */
        if (target_khz < 125000) target_khz = 125000;
        dmesg_log("gov:ondemand backoff");
    }

    if (target_khz != current_khz)
        ramp_to(target_khz);

    (void)metrics;
    sleep_ms(80);
}

static const Governor g = {
    .name = "ondemand",
    .init = ond_init,
    .tick = ond_tick,
};

const Governor *governor_ondemand(void) { return &g; }
