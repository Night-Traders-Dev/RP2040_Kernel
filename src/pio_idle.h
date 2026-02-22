#ifndef PIO_IDLE_H
#define PIO_IDLE_H

/*
 * pio_idle.h  –  CPU idle-time measurement + heartbeat jitter detection
 *
 * Two PIO state machines run autonomously on PIO0, freeing both ARM cores
 * from any timing obligations:
 *
 *   SM0 (idle_measure)    – measures how long Core 0 spends in its
 *                           getchar_timeout_us() spin.  Gives a real-time
 *                           idle fraction that is independent of CPU load.
 *
 *   SM1 (period_measure)  – measures the period between Core 0 heartbeat
 *                           pulses.  A sudden change in consecutive readings
 *                           indicates a PLL transition is in progress.
 *                           Once N readings are within ±CV% the system is
 *                           declared "stable" and the governor may safely
 *                           request the next frequency step.
 *
 * Usage (Core 0 main loop):
 *
 *   pio_idle_init();               // once, before multicore_launch_core1()
 *
 *   while (true) {
 *       pio_idle_heartbeat();      // brief GPIO pulse → SM1 period sample
 *       pio_idle_poll();           // drain FIFOs, update stats
 *       pio_idle_enter();          // tell SM0 "we are idle"
 *       int c = getchar_timeout_us(0);
 *       pio_idle_exit();           // tell SM0 "we have work"
 *       ...
 *   }
 *
 * Governor usage (Core 1):
 *
 *   if (pio_idle_safe_to_scale(0.03f, 3.0f, 4)) {
 *       // OK to call ramp_step()
 *   }
 *   // after ramp_step() succeeds:
 *   pio_idle_notify_freq_change(current_khz);
 */

#include <stdint.h>
#include <stdbool.h>
#include "hardware/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * GPIO pin assignments
 * Change these if the default pins conflict with your hardware.
 * Both are Core 0 OUTPUT pins; PIO reads them as inputs.
 * ------------------------------------------------------------------------- */
#ifndef PIO_IDLE_PIN
#define PIO_IDLE_PIN  20   /* HIGH while Core 0 is in getchar spin-wait   */
#endif
#ifndef PIO_HB_PIN
#define PIO_HB_PIN    21   /* Brief HIGH pulse once per main-loop tick     */
#endif

/* -------------------------------------------------------------------------
 * Snapshot structure filled by pio_idle_poll() / pio_idle_get_stats()
 * ------------------------------------------------------------------------- */
typedef struct {
    /* ----- SM0: idle measurement ----- */
    uint32_t idle_ticks;        /* raw PIO ticks from the last idle window  */
    float    idle_fraction;     /* EMA of idle/total time,  0.0 – 1.0       */

    /* ----- SM1: heartbeat / jitter ----- */
    uint32_t hb_period_ticks;   /* latest LOW-phase measurement (ticks)     */
    uint32_t hb_period_prev;    /* one-sample-ago measurement               */
    int32_t  hb_jitter_ticks;   /* signed delta (current − previous)        */
    float    hb_jitter_pct;     /* |delta| / previous × 100  (%)            */

    /* ----- Stability arbiter ----- */
    uint32_t stable_count;      /* consecutive "stable" HB samples seen     */
    bool     safe_to_scale;     /* cached result of the default thresholds  */
} pio_idle_stats_t;


/* =========================================================================
 * Lifecycle
 * ========================================================================= */

/**
 * pio_idle_init() – install PIO programs, claim SM0+SM1 on PIO0, configure
 * GPIOs and start both state machines.
 *
 * Call ONCE after stdio_init_all() / dmesg_init(), BEFORE
 * multicore_launch_core1().  Idempotent.
 */
void pio_idle_init(void);

/**
 * pio_idle_poll() – drain both RX FIFOs and update the internal stats
 * snapshot.  Non-blocking; typically < 2 µs.
 *
 * May be called from either core; internally protected by a critical section.
 * Call at least once per main-loop iteration from Core 0 for best latency.
 */
void pio_idle_poll(void);

/**
 * pio_idle_get_stats() – copy the latest snapshot into *out.
 * Thread-safe.
 */
void pio_idle_get_stats(pio_idle_stats_t *out);


/* =========================================================================
 * Core 0 GPIO helpers  (inlined for minimum overhead)
 * ========================================================================= */

/** Set IDLE_PIN HIGH – call just before getchar_timeout_us(). */
static inline void pio_idle_enter(void)
{
    gpio_put(PIO_IDLE_PIN, 1);
}

/** Clear IDLE_PIN – call as soon as real work is detected. */
static inline void pio_idle_exit(void)
{
    gpio_put(PIO_IDLE_PIN, 0);
}

/**
 * Emit one heartbeat pulse on PIO_HB_PIN.
 * The pulse is ≥ 8 sys-clock cycles wide so SM1 sees it at any frequency
 * up to the RP2040 maximum.  Call once per main-loop iteration.
 */
static inline void pio_idle_heartbeat(void)
{
    gpio_put(PIO_HB_PIN, 1);
    /* 8 NOPs ≈ 30 ns @ 264 MHz; visible to PIO at clkdiv = 1 */
    __asm volatile(
        "nop\nnop\nnop\nnop\n"
        "nop\nnop\nnop\nnop\n"
        ::: "memory"
    );
    gpio_put(PIO_HB_PIN, 0);
}


/* =========================================================================
 * Unit conversion
 * ========================================================================= */

/**
 * Convert a raw PIO tick count to microseconds.
 *
 * Both SM loops execute 2 instructions per tick (jmp pin + jmp x--),
 * so one tick = 2 sys-clock cycles at clkdiv = 1:
 *
 *   us = ticks × 2 cycles/tick × 1 000 000 / (sys_khz × 1000 cycles/s)
 *      = ticks × 2000 / sys_khz
 */
float pio_idle_ticks_to_us(uint32_t ticks, uint32_t sys_khz);


/* =========================================================================
 * Frequency-change integration
 * ========================================================================= */

/**
 * pio_idle_notify_freq_change(new_khz)
 *
 * MUST be called from Core 1 immediately after a successful ramp_step().
 * Resets the heartbeat window so stale readings from the old frequency do
 * not pollute the new stability baseline, and activates a settle window
 * during which jitter readings are suppressed.
 */
void pio_idle_notify_freq_change(uint32_t new_khz);

/**
 * pio_idle_update_clkdiv(sys_khz)
 *
 * Updates the internal conversion factor used by ticks_to_us().
 * Currently keeps clkdiv = 1.0 for maximum resolution; edit the body to
 * set a fixed-frequency timebase if preferred.
 */
void pio_idle_update_clkdiv(uint32_t sys_khz);


/* =========================================================================
 * Governor arbiter
 * ========================================================================= */

/**
 * pio_idle_safe_to_scale(idle_thresh, jitter_thresh, min_stable)
 *
 * Returns true when all of:
 *   1. Heartbeat period CV is below STABLE_CV_PCT for the last N readings.
 *   2. Most-recent |jitter_pct| ≤ jitter_thresh (%).
 *   3. stable_count ≥ min_stable.
 *
 * The idle_thresh parameter is advisory; the primary gate is stability.
 * Returns true unconditionally if pio_idle_init() has not been called
 * (failsafe: don't block the governor on an uninitialised subsystem).
 *
 * Suggested defaults: idle_thresh=0.03f, jitter_thresh=3.0f, min_stable=4.
 */
bool pio_idle_safe_to_scale(float idle_thresh, float jitter_thresh,
                            uint32_t min_stable);

#ifdef __cplusplus
}
#endif

#endif /* PIO_IDLE_H */
