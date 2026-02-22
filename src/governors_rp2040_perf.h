#ifndef GOVERNORS_RP2040_PERF_H
#define GOVERNORS_RP2040_PERF_H

#ifdef __cplusplus
extern "C" {
#endif

int rp2040_perf_set_param(const char *name, double val);
int rp2040_perf_get_param(const char *name, double *out);
void rp2040_perf_print_params(void);
void rp2040_perf_list_params(void);
/* Persisted/tunable idle target (kHz) may be queried via get/set APIs */
int rp2040_perf_set_idle_target_khz(uint32_t khz);
uint32_t rp2040_perf_get_idle_target_khz(void);

#ifdef __cplusplus
}
#endif

#endif
