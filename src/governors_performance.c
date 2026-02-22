#include <stdio.h>
#include "pico/stdlib.h"
#include "system.h"
#include "hardware/vreg.h"
#include "dmesg.h"
#include "governors.h"

/* Performance governor: always aim for max frequency */

static uint32_t last_logged_target = 0;  /* Track to reduce logging spam */

static void perf_init(void)
{
    target_khz = MIN_KHZ;  /* Start at idle frequency and ramp up */
    last_logged_target = MIN_KHZ;
    dmesg_log("gov:performance initialized at idle");
}

static void perf_tick(const metrics_agg_t *metrics)
{
    core1_wdt_ping++;
    (void)metrics;
    
    /* Ensure we're always targeting MAX */
    if (target_khz != MAX_KHZ) {
        /* Pre-warm VREG for highest targets before switching */
        if (MAX_KHZ > 250000) {
#if defined(VREG_VOLTAGE_1_35)
            vreg_set_voltage(VREG_VOLTAGE_1_35);
#else
            vreg_set_voltage(VREG_VOLTAGE_1_30);
#endif
        } else if (MAX_KHZ > 200000) {
            vreg_set_voltage(VREG_VOLTAGE_1_20);
        }
        target_khz = MAX_KHZ;
        if (target_khz != last_logged_target) {
            dmesg_log("gov:performance ramp to MAX");
            last_logged_target = target_khz;
        }
    }
    
    /* Non-blocking: ramp one step at a time instead of blocking */
    if (target_khz != current_khz)
        ramp_step(target_khz);
    
    sleep_ms(200);
}

static const Governor g = {
    .name = "performance",
    .init = perf_init,
    .tick = perf_tick,
};

const Governor *governor_performance(void) { return &g; }
