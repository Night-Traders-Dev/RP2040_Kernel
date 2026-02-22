#ifndef SYSTEM_H
#define SYSTEM_H

#include <stdint.h>
#include <stdbool.h>

/* Frequency limits (kHz) */
#define MIN_KHZ 125000
#define MAX_KHZ 264000

/* Temperature / ADC */
float read_onboard_temperature(void);

/* Frequency ramping
 *
 * ramp_step() -- advance one RAMP_STEP_KHZ step toward new_khz.
 *   Handles voltage sequencing (raise before up, lower after down).
 *   Does NOT sleep. Returns true when target is reached.
 *   Safe to call from Core 1.
 *
 * ramp_to() -- blocking convenience wrapper that drives ramp_step()
 *   with RAMP_DELAY_MS pacing and core1_wdt_ping updates per step.
 */
bool ramp_step(uint32_t new_khz);
void ramp_to(uint32_t new_khz);

/* Display */
void print_stats(void);
const char *voltage_label(uint32_t mv);

/* Core 1 entry point */
void core1_entry(void);

/* Shared state (32-bit aligned; M0+ word access is atomic) */
extern volatile uint32_t target_khz;
extern volatile uint32_t current_khz;
extern volatile bool     live_stats;
extern volatile uint32_t core1_wdt_ping;
extern volatile bool     throttle_active;
extern volatile uint32_t current_voltage_mv;
extern volatile uint32_t stat_period_ms;

#endif