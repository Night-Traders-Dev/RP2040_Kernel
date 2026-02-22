#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/sync.h"
#include "hardware/adc.h"
#include "system.h"
#include "dmesg.h"
#include "governors.h"
#include "metrics.h"
#include "uart_log.h"

/* Ramp constants */
#define RAMP_STEP_KHZ        5000
#define RAMP_DELAY_MS        10
#define TEMP_LOG_INTERVAL_MS 30000

/* Shared globals */
volatile uint32_t target_khz        = MAX_KHZ;
volatile uint32_t current_khz       = MIN_KHZ;
volatile bool     live_stats        = false;
volatile uint32_t core1_wdt_ping    = 0;
volatile bool     throttle_active   = false;
volatile uint32_t current_voltage_mv = 1100;
volatile uint32_t stat_period_ms    = 500;

/* --------------------------------------------------------------------------
 * Voltage helpers
 * -------------------------------------------------------------------------- */

const char *voltage_label(uint32_t mv)
{
    switch (mv) {
        case 1100: return "1.10V (default)";
        case 1200: return "1.20V";
        case 1300: return "1.30V";
        case 1350: return "1.35V";
        default:   return "unknown";
    }
}

/* Select the minimum safe VREG setting for a given clock frequency.
 * This is the single authoritative place for voltage/frequency mapping.
 * Call BEFORE raising frequency, AFTER lowering it. */
static void vreg_for_khz(uint32_t khz)
{
#if defined(VREG_VOLTAGE_1_35)
    if (khz > 250000) {
        vreg_set_voltage(VREG_VOLTAGE_1_35);
        current_voltage_mv = 1350;
    } else if (khz > 200000) {
        vreg_set_voltage(VREG_VOLTAGE_1_20);
        current_voltage_mv = 1200;
    } else {
        vreg_set_voltage(VREG_VOLTAGE_DEFAULT);
        current_voltage_mv = 1100;
    }
#else
    if (khz > 250000) {
        vreg_set_voltage(VREG_VOLTAGE_1_30);
        current_voltage_mv = 1300;
    } else if (khz > 200000) {
        vreg_set_voltage(VREG_VOLTAGE_1_20);
        current_voltage_mv = 1200;
    } else {
        vreg_set_voltage(VREG_VOLTAGE_DEFAULT);
        current_voltage_mv = 1100;
    }
#endif
}

/* --------------------------------------------------------------------------
 * find_achievable_khz -- find nearest PLL-achievable frequency
 *
 * Starting from `candidate`, scans 1 kHz at a time in the direction of
 * `target` until check_sys_clock_khz() confirms the PLL can lock.
 *
 * The RP2040 PLL is constrained to:
 *   sys_clk = (XOSC[12MHz] * fbdiv) / (postdiv1 * postdiv2)
 *   VCO = XOSC * fbdiv  must be in [750, 1600] MHz
 *   fbdiv in [16, 320], postdiv1/2 in [1, 7]
 *
 * Many round kHz values (e.g. 145000) simply have no valid divisor
 * combination. This function skips over them rather than giving up.
 *
 * Returns the nearest achievable kHz, which will be <= target when
 * stepping up and >= target when stepping down.
 * -------------------------------------------------------------------------- */
static uint32_t find_achievable_khz(uint32_t candidate, uint32_t target)
{
    bool up = (candidate <= target);
    uint32_t limit = up ? target : candidate;
    uint32_t probe = candidate;

    /* Scan at most ~50 kHz away (50 iterations of 1 kHz) before giving up.
     * In practice the RP2040 always has a valid frequency within a few kHz. */
    for (int i = 0; i < 50; i++) {
        uint vco, pd1, pd2;
        if (check_sys_clock_khz(probe, &vco, &pd1, &pd2))
            return probe;

        if (up) {
            probe++;
            if (probe > limit) break;
        } else {
            if (probe == 0) break;
            probe--;
            if (probe < limit) break;
        }
    }

    /* Fallback: return target itself and let set_sys_clock_khz decide. */
    return target;
}

/* --------------------------------------------------------------------------
 * ramp_step  -- advance exactly one step toward new_khz
 *
 * Voltage sequencing rules:
 *   Ramping UP:   raise voltage BEFORE changing clock (never under-volt)
 *   Ramping DOWN: lower voltage AFTER  changing clock (never over-volt)
 *
 * Non-achievable PLL frequencies are skipped transparently via
 * find_achievable_khz() -- the ramp continues rather than aborting.
 *
 * Returns: true  if target reached (caller can stop looping)
 *          false if more steps remain
 *
 * Safe to call from Core 1. Does NOT sleep -- caller controls pacing.
 * -------------------------------------------------------------------------- */
bool ramp_step(uint32_t new_khz)
{
    if (current_khz == new_khz)
        return true;

    bool stepping_up = (current_khz < new_khz);
    uint32_t candidate;

    if (stepping_up) {
        candidate = current_khz + RAMP_STEP_KHZ;
        if (candidate > new_khz) candidate = new_khz;
    } else {
        candidate = current_khz - RAMP_STEP_KHZ;
        if (candidate < new_khz) candidate = new_khz;
    }

    /* Resolve to the nearest kHz the PLL can actually lock to. */
    uint32_t next_khz = find_achievable_khz(candidate, new_khz);

    if (stepping_up) {
        /* Raise voltage first so the new frequency is always safe. */
        vreg_for_khz(next_khz);
    }
    /* Stepping down: voltage is still sufficient for current_khz; safe. */

    /* Pause the other core for the duration of the PLL reconfiguration.
     * multicore_lockout requires the other core to have called
     * multicore_lockout_victim_init() during startup (done in main). */
    multicore_lockout_start_blocking();
    bool ok = set_sys_clock_khz(next_khz, false);
    multicore_lockout_end_blocking();

    if (!ok) {
        /* check_sys_clock_khz said this was achievable but set failed --
         * the frequency is right on the PLL edge for this silicon.
         * Do NOT update current_khz: the hardware did not move.
         * Pull target_khz back to wherever we actually are so the governor
         * stops retrying this unachievable frequency. */
        char err[96];
        snprintf(err, sizeof(err),
                 "ramp_step: PLL edge at %u kHz -- clamping target to actual %u kHz",
                 next_khz, current_khz);
        dmesg_log(err);
        target_khz = current_khz;   /* tell governor: this is as high as we go */
        return true;                /* stop ramping, current_khz is still correct */
    }

    if (!stepping_up) {
        /* Safe to lower voltage now that the clock is already slower. */
        vreg_for_khz(next_khz);
    }

    current_khz = next_khz;
    return (current_khz == new_khz);
}

/* --------------------------------------------------------------------------
 * ramp_to  -- blocking ramp from current_khz to new_khz
 *
 * Drives ramp_step() in a loop with RAMP_DELAY_MS between steps.
 * Pings core1_wdt_ping at every step so the main-core watchdog monitor
 * (which checks every 5 s) never sees a stale counter during a long ramp.
 *
 * Worst case: 125 -> 265 MHz = 28 steps * 10 ms = ~280 ms.
 * With WDT pings every step the 5 s main-core timeout is never at risk.
 * -------------------------------------------------------------------------- */
void ramp_to(uint32_t new_khz)
{
    if (current_khz == new_khz)
        return;

    /* Clamp to valid range defensively. */
    if (new_khz < MIN_KHZ) new_khz = MIN_KHZ;
    if (new_khz > MAX_KHZ) new_khz = MAX_KHZ;

    char buf[96];
    snprintf(buf, sizeof(buf), "ramp_to: %u -> %u kHz", current_khz, new_khz);
    dmesg_log(buf);

    while (!ramp_step(new_khz)) {
        core1_wdt_ping++;   /* keep main-core WDT counter advancing */
        sleep_ms(RAMP_DELAY_MS);
    }

    /* One final ping + settle delay after the last step. */
    core1_wdt_ping++;
    sleep_ms(RAMP_DELAY_MS);

    snprintf(buf, sizeof(buf), "ramp_to: done %u kHz @ %s",
             current_khz, voltage_label(current_voltage_mv));
    dmesg_log(buf);
}

/* --------------------------------------------------------------------------
 * Temperature / ADC
 * -------------------------------------------------------------------------- */

float read_onboard_temperature(void)
{
    adc_select_input(4);
    const float conversion_factor = 3.3f / (1 << 12);
    float adc_v = (float)adc_read() * conversion_factor;
    return 27.0f - (adc_v - 0.706f) / 0.001721f;
}

/* --------------------------------------------------------------------------
 * Core 1 entry -- governor tick loop
 * -------------------------------------------------------------------------- */

void core1_entry(void)
{
    dmesg_log("Governor started on core1");

    governors_init();
    if (!governors_get_current())
        governors_set_current(governor_rp2040_perf());

    uint32_t last_stat_ms          = to_ms_since_boot(get_absolute_time());
    uint32_t local_gov_tick_count  = 0;
    double   local_gov_tick_avg_ms = 0.0;

    while (true) {
        const Governor *g = governors_get_current();
        metrics_agg_t agg;
        metrics_get_aggregate(&agg, 1);  /* CLEAR metrics each tick so each cycle sees fresh data */

        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (live_stats && (now_ms - last_stat_ms) >= stat_period_ms) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "STAT clk=%.2fMHz target=%.2fMHz temp=%.1fC vreg=%s\n",
                     clock_get_hz(clk_sys) / 1e6f,
                     target_khz / 1000.0f,
                     read_onboard_temperature(),
                     voltage_label(current_voltage_mv));
            if (uart_log_enabled())
                uart_log_send(buf);
            else
                dmesg_log(buf);
            last_stat_ms = now_ms;
        }

        if (g && g->tick) {
            uint64_t t0 = to_us_since_boot(get_absolute_time());
            g->tick(&agg);
            uint64_t t1 = to_us_since_boot(get_absolute_time());
            double delta_ms = (double)(t1 - t0) / 1000.0;

            local_gov_tick_count++;
            local_gov_tick_avg_ms =
                ((local_gov_tick_avg_ms * (local_gov_tick_count - 1)) + delta_ms)
                / local_gov_tick_count;

            kernel_metrics_t snap;
            snap.gov_tick_count  = local_gov_tick_count;
            snap.gov_tick_avg_ms = local_gov_tick_avg_ms;
            snap.last_ts_ms      = to_ms_since_boot(get_absolute_time());
            metrics_publish_kernel(&snap);
        } else {
            sleep_ms(50);
        }
    }
}

/* --------------------------------------------------------------------------
 * Stats (Core 0 only)
 * -------------------------------------------------------------------------- */

void print_stats(void)
{
    printf("\rClock: %6.2f MHz | Target: %6.2f MHz | Temp: %4.1f °C | Vreg: %s    ",
           clock_get_hz(clk_sys) / 1e6f,
           target_khz / 1000.0f,
           read_onboard_temperature(),
           voltage_label(current_voltage_mv));
    fflush(stdout);
}