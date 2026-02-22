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

/* Constants used by ramping/thermal logic (kept local to this module) */
#define RAMP_STEP_KHZ      5000
#define RAMP_DELAY_MS      10
#define TEMP_LOG_INTERVAL_MS  30000

/* Shared globals (previously in main.c) */
volatile uint32_t target_khz  = MAX_KHZ;
volatile uint32_t current_khz = MIN_KHZ;
volatile bool     live_stats  = false;
volatile uint32_t core1_wdt_ping = 0;
volatile bool throttle_active = false;
volatile uint32_t current_voltage_mv = 1100;
volatile uint32_t stat_period_ms = 500;

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

float read_onboard_temperature(void)
{
    adc_select_input(4);

    const float conversion_factor = 3.3f / (1 << 12);
    float adc_v = (float)adc_read() * conversion_factor;
    float temp_c = 27.0f - (adc_v - 0.706f) / 0.001721f;
    return temp_c;
}

void ramp_to(uint32_t new_khz)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "Ramp requested to %u kHz", new_khz);
    dmesg_log(buf);

    while (current_khz != new_khz) {
        uint32_t next_khz = current_khz;
        if (current_khz < new_khz) {
            next_khz += RAMP_STEP_KHZ;
            if (next_khz > new_khz) next_khz = new_khz;
        } else {
            next_khz -= RAMP_STEP_KHZ;
            if (next_khz < new_khz) next_khz = new_khz;
        }

        if (next_khz > current_khz) {
            /* Increase voltage before raising frequency.
               Prefer 1.35V if the SDK exposes it; otherwise use 1.30V.
               Then fallback to 1.20V for mid-range. */
#if defined(VREG_VOLTAGE_1_35)
            if (next_khz > 250000) {
                vreg_set_voltage(VREG_VOLTAGE_1_35);
                current_voltage_mv = 1350;
            } else if (next_khz > 200000) {
                vreg_set_voltage(VREG_VOLTAGE_1_20);
                current_voltage_mv = 1200;
            }
#else
            if (next_khz > 250000) {
                vreg_set_voltage(VREG_VOLTAGE_1_30);
                current_voltage_mv = 1300;
            } else if (next_khz > 200000) {
                vreg_set_voltage(VREG_VOLTAGE_1_20);
                current_voltage_mv = 1200;
            }
#endif
        }

        multicore_lockout_start_blocking();
        set_sys_clock_khz(next_khz, false);
        multicore_lockout_end_blocking();

        if (next_khz < current_khz) {
            if (next_khz <= 200000) {
                vreg_set_voltage(VREG_VOLTAGE_DEFAULT);
                current_voltage_mv = 1100;
            } else if (next_khz <= 250000) {
                current_voltage_mv = 1200;
            }
        }

        current_khz = next_khz;
        sleep_ms(RAMP_DELAY_MS);
    }

    snprintf(buf, sizeof(buf), "Clock set to %u kHz @ %s",
             current_khz, voltage_label(current_voltage_mv));
    dmesg_log(buf);
}

/* core1 now delegates to a governor tick function; this keeps a small
   wrapper here so the core1 entrypoint stays stable. */
void core1_entry(void)
{
    dmesg_log("Governor started on core1");

    /* Initialize governors and select default if none set */
    governors_init();
    if (!governors_get_current())
        governors_set_current(governor_rp2040_perf());

    uint32_t last_stat_ms = to_ms_since_boot(get_absolute_time());
    /* Local accumulators for kernel tick timing (kept per-core to avoid
     * contention). We publish a snapshot after each tick so governors can
     * make decisions based on kernel behaviour. */
    uint32_t local_gov_tick_count = 0;
    double   local_gov_tick_avg_ms = 0.0;

    while (true) {
        const Governor *g = governors_get_current();
        metrics_agg_t agg;
        metrics_get_aggregate(&agg, 0); /* peek at current metrics */

        /* Non-blocking live stats: publish over DMA-UART if enabled so stats
         * continue while apps or main core are busy. This avoids calling
         * printf from core1. */
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (live_stats && (now_ms - last_stat_ms) >= stat_period_ms) {
            char buf[128];
            snprintf(buf, sizeof(buf), "STAT clk=%.2fMHz target=%.2fMHz temp=%.1fC vreg=%s\n",
                     clock_get_hz(clk_sys) / 1e6f,
                     target_khz / 1000.0f,
                     read_onboard_temperature(),
                     voltage_label(current_voltage_mv));
            if (uart_log_enabled()) {
                uart_log_send(buf);
            } else {
                /* Fallback to dmesg buffer */
                dmesg_log(buf);
            }
            last_stat_ms = now_ms;
        }

        if (g && g->tick) {
            uint64_t t0 = to_us_since_boot(get_absolute_time());
            g->tick(&agg);
            uint64_t t1 = to_us_since_boot(get_absolute_time());
            double delta_ms = (double)(t1 - t0) / 1000.0;

            /* update local running average */
            local_gov_tick_count++;
            local_gov_tick_avg_ms = ((local_gov_tick_avg_ms * (local_gov_tick_count - 1)) + delta_ms) / local_gov_tick_count;

            /* publish small kernel snapshot for governors to consume */
            kernel_metrics_t snap;
            snap.gov_tick_count = local_gov_tick_count;
            snap.gov_tick_avg_ms = local_gov_tick_avg_ms;
            snap.last_ts_ms = to_ms_since_boot(get_absolute_time());
            metrics_publish_kernel(&snap);
        } else {
            sleep_ms(50);
        }
    }
}

void print_stats(void)
{
    printf("\rClock: %6.2f MHz | Target: %6.2f MHz | Temp: %4.1f °C | Vreg: %s    ",
           clock_get_hz(clk_sys) / 1e6f,
           target_khz / 1000.0f,
           read_onboard_temperature(),
           voltage_label(current_voltage_mv));
    fflush(stdout);
}
