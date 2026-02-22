#ifndef SYSTEM_H
#define SYSTEM_H

#include <stdint.h>
#include <stdbool.h>

/* Frequency limits (kHz) */
#define MIN_KHZ 125000
#define MAX_KHZ 265000

float read_onboard_temperature(void);
void ramp_to(uint32_t new_khz);
void core1_entry(void);
void print_stats(void);
const char *voltage_label(uint32_t mv);

extern volatile uint32_t target_khz;
extern volatile uint32_t current_khz;
extern volatile bool     live_stats;
extern volatile uint32_t core1_wdt_ping;
extern volatile bool     throttle_active;
extern volatile uint32_t current_voltage_mv;
extern volatile uint32_t stat_period_ms;

#endif
