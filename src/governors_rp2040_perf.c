#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/vreg.h"
#include "system.h"
#include "dmesg.h"
#include "governors.h"
#include "metrics.h"
#include "pico/time.h"
#include "governors_rp2040_perf.h"
#include <string.h>
#include "persist.h"

/* RP2040-optimized high-performance governor
 * Goals:
 *  - Reach `MAX_KHZ` aggressively but safely
 *  - Pre-set VREG for quicker stable ramp
 *  - Back off quickly on thermal excursion
 *  - Keep tick short and cooperative
 */

/* Tunable parameters (adjustable at runtime via CLI) */
static struct {
    uint32_t cooldown_ms;
    uint32_t ramp_up_cooldown_ms;
    double   thr_high_intensity;
    double   thr_med_intensity;
    double   thr_low_intensity;
    double   dur_high_ms;
    double   dur_med_ms;
    double   dur_short_ms;
    double   temp_backoff_C;
    double   temp_restore_C;
    uint32_t backoff_target_khz;
    uint32_t idle_target_khz;
    uint32_t idle_timeout_ms;
} rp_params = {
    .cooldown_ms = 2000,
    .ramp_up_cooldown_ms = 500,
    .thr_high_intensity = 80.0,
    .thr_med_intensity = 60.0,
    .thr_low_intensity = 20.0,
    .dur_high_ms = 500.0,
    .dur_med_ms = 250.0,
    .dur_short_ms = 200.0,
    .temp_backoff_C = 72.0,
    .temp_restore_C = 65.0,
    .backoff_target_khz = 200000,
    .idle_target_khz = 100000,
    .idle_timeout_ms = 5000,
};

/* Parameter helpers */
int rp2040_perf_set_param(const char *name, double val)
{
    if (!name) return -1;
    /* simple validation and set, then persist */
    if (strcmp(name, "cooldown_ms") == 0) { rp_params.cooldown_ms = (uint32_t)val; goto persist_and_ok; }
    if (strcmp(name, "ramp_up_cooldown_ms") == 0) {
        if (val < 100 || val > 5000) return -2;  /* sanity: 100ms - 5s */
        rp_params.ramp_up_cooldown_ms = (uint32_t)val; goto persist_and_ok; }
    if (strcmp(name, "thr_high_intensity") == 0) { rp_params.thr_high_intensity = val; goto persist_and_ok; }
    if (strcmp(name, "thr_med_intensity") == 0) { rp_params.thr_med_intensity = val; goto persist_and_ok; }
    if (strcmp(name, "thr_low_intensity") == 0) { rp_params.thr_low_intensity = val; goto persist_and_ok; }
    if (strcmp(name, "dur_high_ms") == 0) { rp_params.dur_high_ms = val; goto persist_and_ok; }
    if (strcmp(name, "dur_med_ms") == 0) { rp_params.dur_med_ms = val; goto persist_and_ok; }
    if (strcmp(name, "dur_short_ms") == 0) { rp_params.dur_short_ms = val; goto persist_and_ok; }
    if (strcmp(name, "temp_backoff_C") == 0) { rp_params.temp_backoff_C = val; goto persist_and_ok; }
    if (strcmp(name, "temp_restore_C") == 0) { rp_params.temp_restore_C = val; goto persist_and_ok; }
    if (strcmp(name, "backoff_target_khz") == 0) {
        if (val < MIN_KHZ || val > MAX_KHZ) return -2;
        rp_params.backoff_target_khz = (uint32_t)val; goto persist_and_ok; }
    if (strcmp(name, "idle_target_khz") == 0) {
        if (val < MIN_KHZ || val > MAX_KHZ) return -2;
        rp_params.idle_target_khz = (uint32_t)val; goto persist_and_ok; }
    if (strcmp(name, "idle_timeout_ms") == 0) {
        if (val < 1000 || val > 60000) return -2;  /* sanity: 1-60 seconds */
        rp_params.idle_timeout_ms = (uint32_t)val; goto persist_and_ok; }
    return -1;

persist_and_ok:
    persist_save_rp_params(&rp_params, sizeof(rp_params));
    return 0;
}

int rp2040_perf_get_param(const char *name, double *out)
{
    if (!name || !out) return -1;
    if (strcmp(name, "cooldown_ms") == 0) { *out = rp_params.cooldown_ms; return 0; }
    if (strcmp(name, "ramp_up_cooldown_ms") == 0) { *out = rp_params.ramp_up_cooldown_ms; return 0; }
    if (strcmp(name, "thr_high_intensity") == 0) { *out = rp_params.thr_high_intensity; return 0; }
    if (strcmp(name, "thr_med_intensity") == 0) { *out = rp_params.thr_med_intensity; return 0; }
    if (strcmp(name, "thr_low_intensity") == 0) { *out = rp_params.thr_low_intensity; return 0; }
    if (strcmp(name, "dur_high_ms") == 0) { *out = rp_params.dur_high_ms; return 0; }
    if (strcmp(name, "dur_med_ms") == 0) { *out = rp_params.dur_med_ms; return 0; }
    if (strcmp(name, "dur_short_ms") == 0) { *out = rp_params.dur_short_ms; return 0; }
    if (strcmp(name, "temp_backoff_C") == 0) { *out = rp_params.temp_backoff_C; return 0; }
    if (strcmp(name, "temp_restore_C") == 0) { *out = rp_params.temp_restore_C; return 0; }
    if (strcmp(name, "backoff_target_khz") == 0) { *out = rp_params.backoff_target_khz; return 0; }
    if (strcmp(name, "idle_target_khz") == 0) { *out = rp_params.idle_target_khz; return 0; }
    if (strcmp(name, "idle_timeout_ms") == 0) { *out = rp_params.idle_timeout_ms; return 0; }
    return -1;
}

void rp2040_perf_print_params(void)
{
    printf("rp2040_perf parameters:\n");
    printf("  cooldown_ms         : %u\n", rp_params.cooldown_ms);
    printf("  thr_high_intensity  : %.1f\n", rp_params.thr_high_intensity);
    printf("  thr_med_intensity   : %.1f\n", rp_params.thr_med_intensity);
    printf("  thr_low_intensity   : %.1f\n", rp_params.thr_low_intensity);
    printf("  dur_high_ms         : %.1f\n", rp_params.dur_high_ms);
    printf("  dur_med_ms          : %.1f\n", rp_params.dur_med_ms);
    printf("  dur_short_ms        : %.1f\n", rp_params.dur_short_ms);
    printf("  temp_backoff_C      : %.1f\n", rp_params.temp_backoff_C);
    printf("  temp_restore_C      : %.1f\n", rp_params.temp_restore_C);
    printf("  backoff_target_khz  : %u\n", rp_params.backoff_target_khz);
    printf("  idle_target_khz     : %u\n", rp_params.idle_target_khz);
    printf("  idle_timeout_ms     : %u (sustained inactivity before idle)\n", rp_params.idle_timeout_ms);
    printf("  ramp_up_cooldown_ms : %u (fast ramp-up on high activity)\n", rp_params.ramp_up_cooldown_ms);
}

void rp2040_perf_list_params(void)
{
    printf("Available params for rp2040_perf:\n");
    printf("  cooldown_ms\n");
    printf("  thr_high_intensity\n");
    printf("  thr_med_intensity\n");
    printf("  thr_low_intensity\n");
    printf("  dur_high_ms\n");
    printf("  dur_med_ms\n");
    printf("  dur_short_ms\n");
    printf("  temp_backoff_C\n");
    printf("  temp_restore_C\n");
    printf("  backoff_target_khz\n");
    printf("  idle_target_khz\n");
    printf("  idle_timeout_ms\n");
    printf("  ramp_up_cooldown_ms\n");
}

/* runtime stats */
static uint32_t rp_last_adjust_ms = 0;
static uint32_t rp_last_target_set = 0;
static uint32_t rp_adjust_count = 0;
static uint32_t rp_idle_switch_count = 0;
static uint32_t rp_last_idle_ms = 0;
static uint32_t rp_last_activity_ms = 0;
static bool rp_in_idle_state = false;
static uint32_t rp_ramp_up_detection_time = 0;

static void rp_export_stats(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    snprintf(buf, len, "rp2040_perf: adjustments=%u last_target=%ukHz idle_state=%s idle_switches=%u",
             rp_adjust_count, rp_last_target_set, rp_in_idle_state ? "YES" : "no", rp_idle_switch_count);
}

static void rp_init(void)
{
    /* Ensure metrics subsystem is ready */
    metrics_init();

    /* Attempt to load persisted parameters if present */
    if (persist_load_rp_params(&rp_params, sizeof(rp_params)) > 0) {
        dmesg_log("gov:rp2040_perf loaded persisted params");
    }

    /* Pre-warm voltage to highest needed for MAX_KHZ */
    if (MAX_KHZ > 250000) {
#if defined(VREG_VOLTAGE_1_35)
        vreg_set_voltage(VREG_VOLTAGE_1_35);
        current_voltage_mv = 1350;
#else
        vreg_set_voltage(VREG_VOLTAGE_1_30);
        current_voltage_mv = 1300;
#endif
    } else if (MAX_KHZ > 200000) {
        vreg_set_voltage(VREG_VOLTAGE_1_20);
        current_voltage_mv = 1200;
    }

    /* Start at conservative idle frequency; let activity ramp us up to MAX
       This ensures we properly detect ramp-up and log transitions. */
    target_khz = rp_params.idle_target_khz;
    rp_last_activity_ms = to_ms_since_boot(get_absolute_time());
    rp_in_idle_state = true;  /* starting in idle, exit when activity detected */
    dmesg_log("gov:rp2040_perf initialized (starting at idle target)");
}

static void rp_tick(const metrics_agg_t *metrics)
{
    core1_wdt_ping++;
    /* Proactive adjustments based on app-submitted metrics */
    metrics_agg_t agg;
    uint32_t samples = 0;
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (metrics) {
        agg = *metrics;
        samples = agg.count;
    } else {
        samples = metrics_get_aggregate(&agg, 0); /* peek only */
    }
    /* last_adjust_ms is module-scoped so export_stats can access it */
    const uint32_t COOLDOWN_MS = rp_params.cooldown_ms; /* minimum time between target changes */
    const uint32_t RAMP_UP_COOLDOWN_MS = rp_params.ramp_up_cooldown_ms; /* faster cooldown for scale-up */

    /* Update activity tracking: if we see metrics, record activity time */
    if (samples > 0) {
        rp_last_activity_ms = now_ms;
        rp_ramp_up_detection_time = now_ms;  /* reset ramp-up timer on new activity */
    }

    if (samples > 0 && now_ms - rp_last_adjust_ms > COOLDOWN_MS) {
        uint32_t new_target = target_khz;
        bool should_be_idle = false;
        bool is_ramp_up = false;

        /* Detect high activity for aggressive ramp-up:
         * Trigger if either:
         * - Very sustained high intensity (80%+ for 500ms+)
         * - Extremely high intensity burst (90%+ regardless of duration)
         */
        bool high_activity = (agg.avg_intensity >= 90.0) ||
                            (agg.avg_intensity >= rp_params.thr_high_intensity && 
                             agg.avg_duration_ms >= rp_params.dur_high_ms);
        
        /* Debug: log what we see */
        char debug_buf[128];
        snprintf(debug_buf, sizeof(debug_buf), 
                 "gov:tick samples=%u intensity=%.1f%% duration=%.0fms high=%u idle=%u",
                 samples, agg.avg_intensity, agg.avg_duration_ms, high_activity, rp_in_idle_state);
        dmesg_log(debug_buf);
        
        /* Exit idle state aggressively when high activity detected */
        if (rp_in_idle_state && high_activity) {
            rp_in_idle_state = false;
            dmesg_log("gov:rp2040_perf exiting idle on high activity");
            /* Pre-warm VREG for aggressive ramp-up */
            if (MAX_KHZ > 250000) {
#if defined(VREG_VOLTAGE_1_35)
                vreg_set_voltage(VREG_VOLTAGE_1_35);
                current_voltage_mv = 1350;
#else
                vreg_set_voltage(VREG_VOLTAGE_1_30);
                current_voltage_mv = 1300;
#endif
            } else if (MAX_KHZ > 200000) {
                vreg_set_voltage(VREG_VOLTAGE_1_20);
                current_voltage_mv = 1200;
            }
        }

        /* Use intensity + duration to choose appropriate step */
        if (high_activity) {
            new_target = MAX_KHZ; /* sustained high intensity -> max */
            is_ramp_up = (new_target > target_khz);
        } else if (agg.avg_intensity >= rp_params.thr_med_intensity && agg.avg_duration_ms >= rp_params.dur_med_ms) {
            /* medium-high sustained load -> high step (intermediate for faster ramp-up) */
            uint32_t high_step = (MAX_KHZ > 230000) ? 230000 : MAX_KHZ;
            new_target = high_step;
            is_ramp_up = (new_target > target_khz);
        } else if (agg.avg_intensity <= rp_params.thr_low_intensity && agg.avg_duration_ms < rp_params.dur_short_ms) {
            /* very light/short workload -> drop to idle frequency */
            new_target = rp_params.idle_target_khz;
            should_be_idle = true;
        } else if (agg.avg_intensity <= 40.0) {
            /* moderate-low intensity -> favor idle frequency to save power */
            new_target = rp_params.idle_target_khz;
            should_be_idle = true;
        } else {
            /* Otherwise maintain current target during medium load */
            new_target = target_khz;
        }

        /* Apply adaptive cooldown: faster for ramp-up, slower for ramp-down */
        uint32_t effective_cooldown = (is_ramp_up && !rp_in_idle_state) ? RAMP_UP_COOLDOWN_MS : COOLDOWN_MS;

        if (new_target != target_khz && now_ms - rp_last_adjust_ms > effective_cooldown) {
            char buf[128];
            const char *ramp_dir = (new_target > target_khz) ? "up" : "down";
            snprintf(buf, sizeof(buf), "gov:rp2040_perf metrics ramp-%s-> %u (i=%.1f%% dur=%.0fms)",
                     ramp_dir, new_target, agg.avg_intensity, agg.avg_duration_ms);
            dmesg_log(buf);
            target_khz = new_target;
            rp_last_adjust_ms = now_ms;
            rp_last_target_set = new_target;
            rp_adjust_count++;
            if (should_be_idle) {
                rp_last_idle_ms = now_ms;
                rp_idle_switch_count++;
                rp_in_idle_state = true;
            }
        }
    } else if (samples == 0 && !rp_in_idle_state) {
        /* NO metrics for extended period: aggressive idle entry */
        uint32_t inactivity_ms = now_ms - rp_last_activity_ms;
        if (inactivity_ms >= rp_params.idle_timeout_ms && now_ms - rp_last_adjust_ms > COOLDOWN_MS) {
            target_khz = rp_params.idle_target_khz;
            rp_last_adjust_ms = now_ms;
            rp_last_target_set = target_khz;
            rp_last_idle_ms = now_ms;
            rp_idle_switch_count++;
            rp_adjust_count++;
            rp_in_idle_state = true;
            char buf[128];
            snprintf(buf, sizeof(buf), "gov:rp2040_perf idle timeout (%ums inactivity) -> %ukHz",
                     inactivity_ms, target_khz);
            dmesg_log(buf);
        }
    }

    /* Monitor temperature; if too hot, reduce target quickly */
    float temp = read_onboard_temperature();
    if (temp > rp_params.temp_backoff_C && target_khz > rp_params.backoff_target_khz) {
        target_khz = rp_params.backoff_target_khz;
        rp_in_idle_state = false;  /* exiting idle on thermal backoff */
        dmesg_log("gov:rp2040_perf thermal backoff (param)");
        rp_last_adjust_ms = now_ms;
        rp_last_target_set = target_khz;
        rp_adjust_count++;
    } else if (temp < rp_params.temp_restore_C && target_khz < MAX_KHZ && !rp_in_idle_state) {
        /* restore aggressive target only if cooled AND not in idle state */
        target_khz = MAX_KHZ;
        dmesg_log("gov:rp2040_perf restoring target -> MAX");
    }

    /* If core0 hasn't reached target, ask it to ramp one step (non-blocking).
       This allows the loop to remain responsive and process new metrics
       even while frequency is ramping. */
    if (target_khz != current_khz) {
        ramp_step(target_khz);
    }

    /* Small sleep to keep tick responsive but not spin */
    sleep_ms(40);
}

static const Governor g = {
    .name = "rp2040_perf",
    .init = rp_init,
    .tick = rp_tick,
    .export_stats = rp_export_stats,
};

const Governor *governor_rp2040_perf(void) { return &g; }
