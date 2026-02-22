#ifndef METRICS_H
#define METRICS_H

#include <stdint.h>

typedef struct {
    uint32_t count;
    double   avg_workload;   /* arbitrary units provided by app */
    double   avg_intensity;  /* 0..100 percent style */
    double   avg_duration_ms;
    uint32_t last_ts_ms;     /* ms since boot of last sample */
} metrics_agg_t;

/* Kernel-level snapshot that the kernel publishes for governors to consume.
 * Keep this small and copy-friendly so governors can read it without heavy
 * locking. */
typedef struct {
    uint32_t gov_tick_count;    /* number of tick measurements */
    double   gov_tick_avg_ms;   /* running average tick duration (ms) */
    uint32_t last_ts_ms;        /* ms since boot of last measurement */
} kernel_metrics_t;

/* Initialize metrics subsystem (idempotent) */
void metrics_init(void);

/* Submit a sample describing recent work. Called by application code. */
void metrics_submit(uint32_t workload, uint32_t intensity, uint32_t duration_ms);

/* Compute aggregated statistics. If `clear` is non-zero the stored samples are
 * consumed (reset). Returns number of samples aggregated (0 if none).
 */
uint32_t metrics_get_aggregate(metrics_agg_t *out, int clear);

/* Kernel metrics: publish a fresh snapshot (called by kernel/system code).
 * This copies the provided snapshot into an atomic store for readers. */
void metrics_publish_kernel(const kernel_metrics_t *snap);

/* Retrieve latest kernel snapshot. Returns 1 if a snapshot exists, 0 otherwise. */
int metrics_get_kernel_snapshot(kernel_metrics_t *out);

#endif
