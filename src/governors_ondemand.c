#include <stdio.h>
#include "pico/stdlib.h"
#include "system.h"
#include "dmesg.h"
#include "governors.h"

/* On-demand governor: ramp up aggressively when activity seen, back off slowly.
   For testing we use temperature as proxy for activity. */

static uint64_t last_idle_backoff_us = 0;
static const uint64_t idle_backoff_cooldown_us = 500000;  /* 500ms between idle backoffs */

static void ond_init(void) { 
    last_idle_backoff_us = to_us_since_boot(get_absolute_time());
}

static void ond_tick(const metrics_agg_t *metrics)
{
    core1_wdt_ping++;
    float temp = read_onboard_temperature();
    uint64_t now_us = to_us_since_boot(get_absolute_time());
    
    /* Check idle state first: no metrics or very low activity */
    bool is_idle = (!metrics || metrics->count == 0 || metrics->avg_intensity < 30.0);

    /* Ramp up aggressively if high activity detected */
    if (metrics && metrics->count > 0 && metrics->avg_intensity > 70.0) {
        if (target_khz < MAX_KHZ) target_khz += 30000;
        if (target_khz > MAX_KHZ) target_khz = MAX_KHZ;
        dmesg_log("gov:ondemand ramp up (metrics)");
    } 
    /* Ramp up if cold AND not idle (prevent oscillation) */
    else if (!is_idle && temp < 50.0f && target_khz < MAX_KHZ) {
        target_khz += 20000;
        if (target_khz > MAX_KHZ) target_khz = MAX_KHZ;
        dmesg_log("gov:ondemand ramp up");
    } 
    /* Back off if very hot */
    else if (temp > 65.0f && target_khz > 125000) {
        target_khz -= 10000;
        if (target_khz < 125000) target_khz = 125000;
        dmesg_log("gov:ondemand backoff (hot)");
    }
    /* Idle state: gradually back down, but only when cool */
    else if (is_idle && temp < 48.0f && target_khz > 125000 &&
             (now_us - last_idle_backoff_us >= idle_backoff_cooldown_us)) {
        target_khz -= 10000;  /* Larger step to clear PLL quantization boundaries */
        if (target_khz < 125000) target_khz = 125000;
        last_idle_backoff_us = now_us;
        dmesg_log("gov:ondemand idle backoff");
    }

    /* Non-blocking: ramp one step at a time instead of blocking */
    if (target_khz != current_khz)
        ramp_step(target_khz);

    sleep_ms(80);
}

static const Governor g = {
    .name = "ondemand",
    .init = ond_init,
    .tick = ond_tick,
};

const Governor *governor_ondemand(void) { return &g; }
