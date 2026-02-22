#include <stdio.h>
#include "pico/stdlib.h"
#include "system.h"
#include "hardware/vreg.h"
#include "dmesg.h"
#include "governors.h"

/* Schedutil-style governor: try to follow a 'utilization' estimate.
   Scales frequency proportionally to workload intensity. */

static uint64_t last_high_util_us = 0;
static const uint64_t idle_backoff_cooldown_us = 500000;  /* 500ms between idle backoffs */
static uint64_t last_idle_backoff_us = 0;
static uint32_t last_logged_target = 0;  /* Track to reduce logging spam */

static void sch_init(void) { 
    last_high_util_us = to_us_since_boot(get_absolute_time());
    last_idle_backoff_us = to_us_since_boot(get_absolute_time());
    target_khz = MIN_KHZ;  /* Start at idle frequency */
    last_logged_target = MIN_KHZ;
    dmesg_log("gov:schedutil initialized at idle");
}

static void sch_tick(const metrics_agg_t *metrics)
{
    core1_wdt_ping++;
    float temp = read_onboard_temperature();
    uint64_t now_us = to_us_since_boot(get_absolute_time());

    /* Prefer app-reported utilization if available */
    int util;
    bool has_metrics = (metrics && metrics->count > 0);
    
    if (has_metrics) {
        util = (int)metrics->avg_intensity;
        char buf[80];
        snprintf(buf, sizeof(buf), "gov:schedutil metrics (util=%d%%)", util);
        dmesg_log(buf);
        if (util > 50)
            last_high_util_us = now_us;  /* Track when we last saw meaningful activity */
    } else {
        /* No metrics: use conservative temperature-based estimate with hysteresis */
        util = (int) ( (temp - 32.0f) * 0.5f );  /* More conservative scaling */
    }
    if (util < 0) util = 0;
    if (util > 100) util = 100;

    /* Only update target if change is >5% (hysteresis) to avoid oscillation */
    uint32_t target = MIN_KHZ + (uint32_t)((MAX_KHZ - MIN_KHZ) * util / 100);
    if (target > MAX_KHZ) target = MAX_KHZ;
    if (target < MIN_KHZ) target = MIN_KHZ;

    /* Hysteresis: only change if >5% difference from current target */
    uint32_t current_target_percent = (target_khz - MIN_KHZ) * 100 / (MAX_KHZ - MIN_KHZ);
    if (target_khz != target && (uint32_t)util > current_target_percent + 5 || (uint32_t)util < current_target_percent - 5) {
        target_khz = target;
        if (target_khz != last_logged_target) {
            char buf[80];
            snprintf(buf, sizeof(buf), "gov:schedutil target -> %u kHz (util=%d%%)", target, util);
            dmesg_log(buf);
            last_logged_target = target_khz;
        }
    }
    
    /* Idle backoff: if no high util seen for 2s and cool, slowly decay */
    if (!has_metrics && util < 20 && temp < 48.0f && target_khz > MIN_KHZ &&
        (now_us - last_high_util_us > 2000000) &&
        (now_us - last_idle_backoff_us >= idle_backoff_cooldown_us)) {
        target_khz -= 10000;
        if (target_khz < MIN_KHZ) target_khz = MIN_KHZ;
        last_idle_backoff_us = now_us;
        if (target_khz != last_logged_target) {
            dmesg_log("gov:schedutil idle backoff");
            last_logged_target = target_khz;
        }
    }

    /* Non-blocking: ramp one step at a time instead of blocking */
    /* Pre-warm VREG on significant upscales */
    if (target_khz > current_khz) {
        if (target_khz > 250000) {
#if defined(VREG_VOLTAGE_1_35)
            vreg_set_voltage(VREG_VOLTAGE_1_35);
#else
            vreg_set_voltage(VREG_VOLTAGE_1_30);
#endif
        } else if (target_khz > 200000) {
            vreg_set_voltage(VREG_VOLTAGE_1_20);
        }
    }

    if (target_khz != current_khz)
        ramp_step(target_khz);

    sleep_ms(60);
}

static const Governor g = {
    .name = "schedutil",
    .init = sch_init,
    .tick = sch_tick,
};

const Governor *governor_schedutil(void) { return &g; }
