#include "metrics.h"
#include <string.h>
#include <stdio.h>
#include "pico/time.h"
#include "pico/sync.h"

/* Kernel snapshot storage */
static kernel_metrics_t kernel_snap;
static mutex_t kernel_lock;
static int kernel_inited = 0;

/* Simple fixed-size ring buffer for submitted metrics */
#define METRICS_BUF_SZ 128

typedef struct {
    uint32_t workload;
    uint32_t intensity;
    uint32_t duration_ms;
    uint32_t ts_ms;
} metric_rec_t;

static metric_rec_t buf[METRICS_BUF_SZ];
static uint32_t head = 0;
static uint32_t tail = 0;
static uint32_t cnt = 0;
static mutex_t metrics_lock;
static int metrics_inited = 0;

void metrics_init(void)
{
    if (metrics_inited) return;
    mutex_init(&metrics_lock);
    head = tail = cnt = 0;
    metrics_inited = 1;
    if (!kernel_inited) {
        mutex_init(&kernel_lock);
        memset(&kernel_snap, 0, sizeof(kernel_snap));
        kernel_inited = 1;
    }
}

void metrics_submit(uint32_t workload, uint32_t intensity, uint32_t duration_ms)
{
    if (!metrics_inited) metrics_init();
    uint32_t ts = to_ms_since_boot(get_absolute_time());
    mutex_enter_blocking(&metrics_lock);
    buf[head].workload = workload;
    buf[head].intensity = intensity;
    buf[head].duration_ms = duration_ms;
    buf[head].ts_ms = ts;
    head = (head + 1) & (METRICS_BUF_SZ - 1);
    if (cnt < METRICS_BUF_SZ) cnt++; else tail = head; /* overwrite oldest */
    mutex_exit(&metrics_lock);
}

uint32_t metrics_get_aggregate(metrics_agg_t *out, int clear)
{
    if (!metrics_inited) metrics_init();
    if (!out) return 0;
    uint32_t local_cnt = 0;
    uint64_t sum_work = 0;
    uint64_t sum_int = 0;
    uint64_t sum_dur = 0;
    uint32_t last_ts = 0;

    mutex_enter_blocking(&metrics_lock);
    uint32_t i = tail;
    for (uint32_t n = 0; n < cnt; ++n) {
        metric_rec_t *r = &buf[i];
        sum_work += r->workload;
        sum_int  += r->intensity;
        sum_dur  += r->duration_ms;
        last_ts = r->ts_ms;
        i = (i + 1) & (METRICS_BUF_SZ - 1);
        local_cnt++;
    }
    if (clear) {
        head = tail = cnt = 0;
    }
    mutex_exit(&metrics_lock);

    if (local_cnt == 0) {
        out->count = 0;
        out->avg_workload = 0.0;
        out->avg_intensity = 0.0;
        out->avg_duration_ms = 0.0;
        out->last_ts_ms = 0;
        return 0;
    }

    out->count = local_cnt;
    out->avg_workload = (double)sum_work / (double)local_cnt;
    out->avg_intensity = (double)sum_int / (double)local_cnt;
    out->avg_duration_ms = (double)sum_dur / (double)local_cnt;
    out->last_ts_ms = last_ts;
    return local_cnt;
}

void metrics_publish_kernel(const kernel_metrics_t *snap)
{
    if (!kernel_inited) {
        mutex_init(&kernel_lock);
        kernel_inited = 1;
    }
    if (!snap) return;
    mutex_enter_blocking(&kernel_lock);
    memcpy(&kernel_snap, snap, sizeof(kernel_snap));
    mutex_exit(&kernel_lock);
}

int metrics_get_kernel_snapshot(kernel_metrics_t *out)
{
    if (!kernel_inited) return 0;
    if (!out) return 0;
    mutex_enter_blocking(&kernel_lock);
    memcpy(out, &kernel_snap, sizeof(kernel_snap));
    mutex_exit(&kernel_lock);
    /* consider snapshot valid if we've seen at least one tick */
    return (kernel_snap.gov_tick_count != 0) ? 1 : 0;
}
