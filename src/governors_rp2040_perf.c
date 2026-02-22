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
} rp_params = {
    .cooldown_ms = 2000,
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
};

/* Parameter helpers */
int rp2040_perf_set_param(const char *name, double val)
{
    if (!name) return -1;
    /* simple validation and set, then persist */
    if (strcmp(name, "cooldown_ms") == 0) { rp_params.cooldown_ms = (uint32_t)val; goto persist_and_ok; }
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
    return -1;

persist_and_ok:
    persist_save_rp_params(&rp_params, sizeof(rp_params));
    return 0;
}

int rp2040_perf_get_param(const char *name, double *out)
{
    if (!name || !out) return -1;
    if (strcmp(name, "cooldown_ms") == 0) { *out = rp_params.cooldown_ms; return 0; }
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
}

/* runtime stats */
static uint32_t rp_last_adjust_ms = 0;
static uint32_t rp_last_target_set = 0;
static uint32_t rp_adjust_count = 0;
static uint32_t rp_idle_switch_count = 0;
static uint32_t rp_last_idle_ms = 0;

static void rp_export_stats(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    snprintf(buf, len, "rp2040_perf: adjustments=%u last_target=%ukHz last_adj_ms=%u idle_switches=%u last_idle_ms=%u",
             rp_adjust_count, rp_last_target_set, rp_last_adjust_ms, rp_idle_switch_count, rp_last_idle_ms);
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

    /* Request the high target immediately; ramp_to on core1 will perform
       a guarded frequency switch on core0. */
    target_khz = MAX_KHZ;
    dmesg_log("gov:rp2040_perf initialized (target requested)");
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

    if (samples > 0 && now_ms - rp_last_adjust_ms > COOLDOWN_MS) {
        uint32_t new_target = target_khz;

        /* Use intensity + duration to choose appropriate step */
        if (agg.avg_intensity >= rp_params.thr_high_intensity && agg.avg_duration_ms >= rp_params.dur_high_ms) {
            new_target = MAX_KHZ; /* sustained high intensity -> max */
        } else if (agg.avg_intensity >= rp_params.thr_med_intensity && agg.avg_duration_ms >= rp_params.dur_med_ms) {
            /* medium-high sustained load -> high step */
            new_target = (MAX_KHZ > 230000) ? 230000 : MAX_KHZ;
        } else if (agg.avg_intensity <= rp_params.thr_low_intensity && agg.avg_duration_ms < rp_params.dur_short_ms) {
            /* very light/short or sustained low-intensity -> drop to idle target to save power */
            new_target = rp_params.idle_target_khz;
        } else if (agg.avg_intensity <= 40.0) {
            /* moderate idle -> also favour idle target to save power */
            new_target = rp_params.idle_target_khz;
        } else {
            /* Otherwise honor existing target */
            new_target = target_khz;
        }

        if (new_target != target_khz) {
            char buf[128];
            snprintf(buf, sizeof(buf), "gov:rp2040_perf metrics-> target %u (i=%.1f%% dur=%.0fms cnt=%u)",
                     new_target, agg.avg_intensity, agg.avg_duration_ms, samples);
            dmesg_log(buf);
            target_khz = new_target;
            rp_last_adjust_ms = now_ms;
            rp_last_target_set = new_target;
            rp_adjust_count++;
            if (new_target == rp_params.idle_target_khz) {
                rp_last_idle_ms = now_ms;
                rp_idle_switch_count++;
            }
        }
    }

    /* Monitor temperature; if too hot, reduce target quickly */
    float temp = read_onboard_temperature();
    if (temp > rp_params.temp_backoff_C && target_khz > rp_params.backoff_target_khz) {
        target_khz = rp_params.backoff_target_khz;
        dmesg_log("gov:rp2040_perf thermal backoff (param)");
        rp_last_adjust_ms = now_ms;
        rp_last_target_set = target_khz;
        rp_adjust_count++;
    } else if (temp < rp_params.temp_restore_C && target_khz < MAX_KHZ) {
        /* restore aggressive target if cooled */
        target_khz = MAX_KHZ;
        dmesg_log("gov:rp2040_perf restoring target -> MAX");
    }

    /* If core0 hasn't reached target, ask it to ramp (fast but safe) */
    if (target_khz != current_khz) {
        ramp_to(target_khz);
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
