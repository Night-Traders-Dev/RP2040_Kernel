#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "benchmark.h"
#include "dmesg.h"
#include "governors.h"
#include "metrics.h"

/* External declarations for live stats display during benchmarks */
extern volatile bool live_stats;
extern void print_stats(void);

/* Simple benchmarking utilities.
 * Benchmarks:
 *  - cpu: tight integer loop counting iterations
 *  - memcpy: repeated memcpy between two buffers
 *  - memset: repeated memset of a buffer
 *  - mem_stream: read sequentially through buffer
 *  - rand_access: random reads across buffer
 */

static const char *bench_names[] = {
    "cpu",
    "memcpy",
    "memset",
    "mem_stream",
    "rand_access",
    "mem_stream_dma",
};

void bench_list(void)
{
    printf("Available benchmarks:\n");
    for (size_t i = 0; i < sizeof(bench_names)/sizeof(bench_names[0]); ++i) {
        printf("  %s\n", bench_names[i]);
    }
}

/* External state accessors */
extern volatile uint32_t current_khz;
extern float read_onboard_temperature(void);

/* Measurement helpers return primary metric and seconds */
static void measure_cpu(uint32_t ms, uint64_t *out_iters, double *out_secs)
{
    char log_buf[128];
    float temp = read_onboard_temperature();
    snprintf(log_buf, sizeof(log_buf), "[bench:cpu] START duration=%ums freq=%uMHz temp=%.1f°C", ms, current_khz/1000, temp);
    dmesg_log(log_buf);
    printf("%s\n", log_buf);
    
    uint64_t start_us = to_us_since_boot(get_absolute_time());
    uint64_t end_us = start_us + (uint64_t)ms * 1000ULL;
    uint64_t last_metric_us = start_us;
    uint64_t last_stats_us = start_us;
    uint64_t last_progress_us = start_us;
    volatile uint32_t acc = 0;
    uint64_t iter = 0;
    uint64_t last_iter_snapshot = 0;
    
    while (to_us_since_boot(get_absolute_time()) < end_us) {
        acc += (uint32_t)(iter ^ (iter << 1));
        iter++;
        
        /* Periodically submit high-intensity metrics (every ~100ms) for governor responsiveness */
        uint64_t now_us = to_us_since_boot(get_absolute_time());
        if (now_us - last_metric_us >= 100000) {
            /* Estimate intensity from iterations in last interval */
            uint64_t iters_done = iter - last_iter_snapshot;
            last_iter_snapshot = iter;
            /* Rough calibration: assume ~5,000,000 iters/100ms ~ 100% */
            double intensity = (double)iters_done / 5000000.0 * 100.0;
            if (intensity < 1.0) intensity = 1.0;
            if (intensity > 100.0) intensity = 100.0;
            metrics_submit(100, (int)intensity, 100);
            last_metric_us = now_us;
            last_progress_us = now_us;
            snprintf(log_buf, sizeof(log_buf), "bench:cpu @%ums iters=%llu intensity=%.0f%% freq=%uMHz", 
                (unsigned)((now_us - start_us) / 1000), (unsigned long long)iter, intensity, current_khz/1000);
            dmesg_log(log_buf);
            sleep_us(100);  /* Yield briefly to allow Core 0 REPL to update stats */
        }
        /* Update stats display every 500ms to match main loop tickrate */
        if (live_stats && (now_us - last_stats_us >= 500000)) {
            print_stats();
            last_stats_us = now_us;
        }
    }
    uint64_t elapsed_us = to_us_since_boot(get_absolute_time()) - start_us;
    double secs = elapsed_us / 1e6;
    double iter_per_sec = iter / secs;
    (void)acc;
    if (out_iters) *out_iters = iter;
    if (out_secs) *out_secs = secs;
    
    snprintf(log_buf, sizeof(log_buf), "[bench:cpu] END iterations=%llu time=%.3fs rate=%.1f Miter/s freq=%uMHz temp=%.1f°C",
        (unsigned long long)iter, secs, iter_per_sec / 1e6, current_khz/1000, read_onboard_temperature());
    dmesg_log(log_buf);
    printf("%s\n", log_buf);
}

/* Memory throughput helpers: use moderate buffer sizes to fit RP2040 RAM */
#define BUF_SIZE (32 * 1024)

static void measure_memcpy(uint32_t ms, double *out_mb, double *out_secs)
{
    char log_buf[128];
    snprintf(log_buf, sizeof(log_buf), "[bench:memcpy] START duration=%ums bufsize=%uKB freq=%uMHz temp=%.1f°C", ms, BUF_SIZE/1024, current_khz/1000, read_onboard_temperature());
    dmesg_log(log_buf);
    printf("%s\n", log_buf);
    
    uint8_t *src = malloc(BUF_SIZE);
    uint8_t *dst = malloc(BUF_SIZE);
    if (!src || !dst) { 
        if (out_mb) *out_mb = 0.0; if (out_secs) *out_secs = 0.0; 
        free(src); free(dst); 
        dmesg_log("[bench:memcpy] FAILED: malloc error");
        return; 
    }
    for (size_t i = 0; i < BUF_SIZE; ++i) src[i] = (uint8_t)i;
    uint64_t start_us = to_us_since_boot(get_absolute_time());
    uint64_t end_us = start_us + (uint64_t)ms * 1000ULL;
    uint64_t last_metric_us = start_us;
    uint64_t last_stats_us = start_us;
    uint64_t last_ops_snapshot = 0;
    uint64_t ops = 0;
    
    while (to_us_since_boot(get_absolute_time()) < end_us) {
        memcpy(dst, src, BUF_SIZE);
        ops++;
        
        /* Periodically submit high-intensity metrics for governor responsiveness */
        uint64_t now_us = to_us_since_boot(get_absolute_time());
        if (now_us - last_metric_us >= 100000) {
            uint64_t ops_done = ops - last_ops_snapshot;
            last_ops_snapshot = ops;
            double bytes = (double)ops_done * BUF_SIZE;
            /* Calibration: assume ~5 MB/100ms represents 100% */
            double intensity = bytes / (5.0 * 1024.0 * 1024.0) * 100.0;
            if (intensity < 1.0) intensity = 1.0;
            if (intensity > 100.0) intensity = 100.0;
            metrics_submit(100, (int)intensity, 100);
            last_metric_us = now_us;
            double mb_so_far = (double)(ops * BUF_SIZE) / (1024.0 * 1024.0);
            snprintf(log_buf, sizeof(log_buf), "bench:memcpy @%ums ops=%llu MB=%.2f intensity=%.0f%% freq=%uMHz", 
                (unsigned)((now_us - start_us) / 1000), (unsigned long long)ops, mb_so_far, intensity, current_khz/1000);
            dmesg_log(log_buf);
            sleep_us(100);  /* Yield briefly to allow Core 0 REPL to update stats */
        }
        /* Update stats display every 500ms to match main loop tickrate */
        if (live_stats && (now_us - last_stats_us >= 500000)) {
            print_stats();
            last_stats_us = now_us;
        }
    }
    uint64_t elapsed_us = to_us_since_boot(get_absolute_time()) - start_us;
    double secs = elapsed_us / 1e6;
    double mb = (double)(ops * BUF_SIZE) / (1024.0 * 1024.0);
    double mb_per_sec = mb / secs;
    if (out_mb) *out_mb = mb;
    if (out_secs) *out_secs = secs;
    free(src); free(dst);
    
    snprintf(log_buf, sizeof(log_buf), "[bench:memcpy] END ops=%llu MB=%.2f time=%.3fs rate=%.2f MB/s freq=%uMHz temp=%.1f°C",
        (unsigned long long)ops, mb, secs, mb_per_sec, current_khz/1000, read_onboard_temperature());
    dmesg_log(log_buf);
    printf("%s\n", log_buf);
}

static void measure_memset(uint32_t ms, double *out_mb, double *out_secs)
{
    char log_buf[128];
    snprintf(log_buf, sizeof(log_buf), "[bench:memset] START duration=%ums bufsize=%uKB freq=%uMHz temp=%.1f°C", ms, BUF_SIZE/1024, current_khz/1000, read_onboard_temperature());
    dmesg_log(log_buf);
    printf("%s\n", log_buf);
    
    uint8_t *buf = malloc(BUF_SIZE);
    if (!buf) { 
        if (out_mb) *out_mb = 0.0; if (out_secs) *out_secs = 0.0; 
        dmesg_log("[bench:memset] FAILED: malloc error");
        return; 
    }
    uint64_t start_us = to_us_since_boot(get_absolute_time());
    uint64_t end_us = start_us + (uint64_t)ms * 1000ULL;
    uint64_t last_metric_us = start_us;
    uint64_t last_stats_us = start_us;
    uint64_t last_ops_snapshot = 0;
    uint64_t ops = 0;
    
    while (to_us_since_boot(get_absolute_time()) < end_us) {
        memset(buf, 0xA5, BUF_SIZE);
        ops++;
        
        /* Periodically submit high-intensity metrics for governor responsiveness */
        uint64_t now_us = to_us_since_boot(get_absolute_time());
        if (now_us - last_metric_us >= 100000) {
            uint64_t ops_done = ops - last_ops_snapshot;
            last_ops_snapshot = ops;
            double bytes = (double)ops_done * BUF_SIZE;
            double intensity = bytes / (5.0 * 1024.0 * 1024.0) * 100.0;
            if (intensity < 1.0) intensity = 1.0;
            if (intensity > 100.0) intensity = 100.0;
            metrics_submit(100, (int)intensity, 100);
            last_metric_us = now_us;
            double mb_so_far = (double)(ops * BUF_SIZE) / (1024.0 * 1024.0);
            snprintf(log_buf, sizeof(log_buf), "bench:memset @%ums ops=%llu MB=%.2f intensity=%.0f%% freq=%uMHz", 
                (unsigned)((now_us - start_us) / 1000), (unsigned long long)ops, mb_so_far, intensity, current_khz/1000);
            dmesg_log(log_buf);
            sleep_us(100);  /* Yield briefly to allow Core 0 REPL to update stats */
        }
        /* Update stats display every 500ms to match main loop tickrate */
        if (live_stats && (now_us - last_stats_us >= 500000)) {
            print_stats();
            last_stats_us = now_us;
        }
    }
    uint64_t elapsed_us = to_us_since_boot(get_absolute_time()) - start_us;
    double secs = elapsed_us / 1e6;
    double mb = (double)(ops * BUF_SIZE) / (1024.0 * 1024.0);
    double mb_per_sec = mb / secs;
    if (out_mb) *out_mb = mb;
    if (out_secs) *out_secs = secs;
    free(buf);
    
    snprintf(log_buf, sizeof(log_buf), "[bench:memset] END ops=%llu MB=%.2f time=%.3fs rate=%.2f MB/s freq=%uMHz temp=%.1f°C",
        (unsigned long long)ops, mb, secs, mb_per_sec, current_khz/1000, read_onboard_temperature());
    dmesg_log(log_buf);
    printf("%s\n", log_buf);
}

static void measure_mem_stream(uint32_t ms, double *out_mb, double *out_secs)
{
    char log_buf[128];
    snprintf(log_buf, sizeof(log_buf), "[bench:mem_stream] START duration=%ums bufsize=%uKB freq=%uMHz temp=%.1f°C", ms, BUF_SIZE/1024, current_khz/1000, read_onboard_temperature());
    dmesg_log(log_buf);
    printf("%s\n", log_buf);
    
    uint8_t *buf = malloc(BUF_SIZE);
    if (!buf) { 
        if (out_mb) *out_mb = 0.0; if (out_secs) *out_secs = 0.0; 
        dmesg_log("[bench:mem_stream] FAILED: malloc error");
        return; 
    }
    for (size_t i = 0; i < BUF_SIZE; ++i) buf[i] = (uint8_t)(i & 0xFF);
    uint64_t start_us = to_us_since_boot(get_absolute_time());
    uint64_t end_us = start_us + (uint64_t)ms * 1000ULL;
    uint64_t last_metric_us = start_us;
    uint64_t last_stats_us = start_us;
    uint64_t last_bytes_snapshot = 0;
    uint64_t bytes = 0;
    
    while (to_us_since_boot(get_absolute_time()) < end_us) {
        for (size_t i = 0; i < BUF_SIZE; ++i) {
            volatile uint8_t v = buf[i]; (void)v;
        }
        bytes += BUF_SIZE;
        
        /* Periodically submit high-intensity metrics for governor responsiveness */
        uint64_t now_us = to_us_since_boot(get_absolute_time());
        if (now_us - last_metric_us >= 100000) {
            uint64_t bytes_done = bytes - last_bytes_snapshot;
            last_bytes_snapshot = bytes;
            double intensity = (double)bytes_done / (5.0 * 1024.0 * 1024.0) * 100.0;
            if (intensity < 1.0) intensity = 1.0;
            if (intensity > 100.0) intensity = 100.0;
            metrics_submit(100, (int)intensity, 100);
            last_metric_us = now_us;
            double mb_so_far = (double)bytes / (1024.0 * 1024.0);
            snprintf(log_buf, sizeof(log_buf), "bench:mem_stream @%ums passes=%llu MB=%.2f intensity=%.0f%% freq=%uMHz", 
                (unsigned)((now_us - start_us) / 1000), (unsigned long long)(bytes / BUF_SIZE), mb_so_far, intensity, current_khz/1000);
            dmesg_log(log_buf);
            sleep_us(100);  /* Yield briefly to allow Core 0 REPL to update stats */
        }
        /* Update stats display every 500ms to match main loop tickrate */
        if (live_stats && (now_us - last_stats_us >= 500000)) {
            print_stats();
            last_stats_us = now_us;
        }
    }
    uint64_t elapsed_us = to_us_since_boot(get_absolute_time()) - start_us;
    double secs = elapsed_us / 1e6;
    double mb = (double)bytes / (1024.0 * 1024.0);
    double mb_per_sec = mb / secs;
    if (out_mb) *out_mb = mb;
    if (out_secs) *out_secs = secs;
    free(buf);
    
    snprintf(log_buf, sizeof(log_buf), "[bench:mem_stream] END passes=%llu MB=%.2f time=%.3fs rate=%.2f MB/s freq=%uMHz temp=%.1f°C",
        (unsigned long long)(bytes / BUF_SIZE), mb, secs, mb_per_sec, current_khz/1000, read_onboard_temperature());
    dmesg_log(log_buf);
    printf("%s\n", log_buf);
}

/* DMA-backed memory stream: repeatedly DMA-copy a buffer to a second buffer
 * measuring throughput while the CPU remains mostly idle between transfers.
 */
#include "hardware/dma.h"

static void measure_mem_stream_dma(uint32_t ms, double *out_mb, double *out_secs)
{
    char log_buf[128];
    snprintf(log_buf, sizeof(log_buf), "[bench:mem_stream_dma] START duration=%ums bufsize=%uKB freq=%uMHz temp=%.1f°C", ms, BUF_SIZE/1024, current_khz/1000, read_onboard_temperature());
    dmesg_log(log_buf);
    printf("%s\n", log_buf);
    
    uint8_t *src = malloc(BUF_SIZE);
    uint8_t *dst = malloc(BUF_SIZE);
    if (!src || !dst) { 
        if (out_mb) *out_mb = 0.0; if (out_secs) *out_secs = 0.0; 
        free(src); free(dst); 
        dmesg_log("[bench:mem_stream_dma] FAILED: malloc error");
        return; 
    }
    for (size_t i = 0; i < BUF_SIZE; ++i) src[i] = (uint8_t)(i & 0xFF);

    int ch = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);

    uint64_t start_us = to_us_since_boot(get_absolute_time());
    uint64_t end_us = start_us + (uint64_t)ms * 1000ULL;
    uint64_t last_metric_us = start_us;
    uint64_t last_stats_us = start_us;
    uint64_t last_ops_snapshot = 0;
    uint64_t ops = 0;
    
    while (to_us_since_boot(get_absolute_time()) < end_us) {
        dma_channel_configure(ch, &c, dst, src, BUF_SIZE, true);
        dma_channel_wait_for_finish_blocking(ch);
        ops++;
        
        /* Periodically submit high-intensity metrics for governor responsiveness */
        uint64_t now_us = to_us_since_boot(get_absolute_time());
        if (now_us - last_metric_us >= 100000) {
            uint64_t ops_done = ops - last_ops_snapshot;
            last_ops_snapshot = ops;
            double intensity = (double)ops_done / 500.0 * 100.0; /* rough scale */
            if (intensity < 1.0) intensity = 1.0;
            if (intensity > 100.0) intensity = 100.0;
            metrics_submit(100, (int)intensity, 100);
            last_metric_us = now_us;
            double mb_so_far = (double)(ops * BUF_SIZE) / (1024.0 * 1024.0);
            snprintf(log_buf, sizeof(log_buf), "bench:mem_stream_dma @%ums ops=%llu MB=%.2f intensity=%.0f%% freq=%uMHz", 
                (unsigned)((now_us - start_us) / 1000), (unsigned long long)ops, mb_so_far, intensity, current_khz/1000);
            dmesg_log(log_buf);
            sleep_us(100);  /* Yield briefly to allow Core 0 REPL to update stats */
        }
        /* Update stats display every 500ms to match main loop tickrate */
        if (live_stats && (now_us - last_stats_us >= 500000)) {
            print_stats();
            last_stats_us = now_us;
        }
    }
    uint64_t elapsed_us = to_us_since_boot(get_absolute_time()) - start_us;
    double secs = elapsed_us / 1e6;
    double mb = (double)(ops * BUF_SIZE) / (1024.0 * 1024.0);
    double mb_per_sec = mb / secs;
    if (out_mb) *out_mb = mb;
    if (out_secs) *out_secs = secs;

    dma_channel_unclaim(ch);
    free(src); free(dst);
    
    snprintf(log_buf, sizeof(log_buf), "[bench:mem_stream_dma] END ops=%llu MB=%.2f time=%.3fs rate=%.2f MB/s freq=%uMHz temp=%.1f°C",
        (unsigned long long)ops, mb, secs, mb_per_sec, current_khz/1000, read_onboard_temperature());
    dmesg_log(log_buf);
    printf("%s\n", log_buf);
}

/* Simple xorshift RNG for random indices */
static uint32_t rng_state = 0x12345678u;
static inline uint32_t rng_next(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

static void measure_rand_access(uint32_t ms, double *out_accesses_k, double *out_secs)
{
    char log_buf[128];
    snprintf(log_buf, sizeof(log_buf), "[bench:rand_access] START duration=%ums bufsize=%uKB freq=%uMHz temp=%.1f°C", ms, BUF_SIZE/1024, current_khz/1000, read_onboard_temperature());
    dmesg_log(log_buf);
    printf("%s\n", log_buf);
    
    uint8_t *buf = malloc(BUF_SIZE);
    if (!buf) { 
        if (out_accesses_k) *out_accesses_k = 0.0; if (out_secs) *out_secs = 0.0; 
        dmesg_log("[bench:rand_access] FAILED: malloc error");
        return; 
    }
    for (size_t i = 0; i < BUF_SIZE; ++i) buf[i] = (uint8_t)(i & 0xFF);
    uint64_t start_us = to_us_since_boot(get_absolute_time());
    uint64_t end_us = start_us + (uint64_t)ms * 1000ULL;
    uint64_t last_metric_us = start_us;
    uint64_t last_stats_us = start_us;
    uint64_t last_access_snapshot = 0;
    uint64_t accesses = 0;
    
    while (to_us_since_boot(get_absolute_time()) < end_us) {
        uint32_t idx = rng_next() % BUF_SIZE;
        volatile uint8_t v = buf[idx]; (void)v;
        accesses++;
        
        /* Periodically submit high-intensity metrics for governor responsiveness */
        uint64_t now_us = to_us_since_boot(get_absolute_time());
        if (now_us - last_metric_us >= 100000) {
            uint64_t acc_done = accesses - last_access_snapshot;
            last_access_snapshot = accesses;
            double intensity = (double)acc_done / 500000.0 * 100.0; /* rough calibration */
            if (intensity < 1.0) intensity = 1.0;
            if (intensity > 100.0) intensity = 100.0;
            metrics_submit(100, (int)intensity, 100);
            last_metric_us = now_us;
            double kacc_so_far = (double)accesses / 1000.0;
            snprintf(log_buf, sizeof(log_buf), "bench:rand_access @%ums acc=%llu Kacc=%.1f intensity=%.0f%% freq=%uMHz", 
                (unsigned)((now_us - start_us) / 1000), (unsigned long long)accesses, kacc_so_far, intensity, current_khz/1000);
            dmesg_log(log_buf);
            sleep_us(100);  /* Yield briefly to allow Core 0 REPL to update stats */
        }
        /* Update stats display every 500ms to match main loop tickrate */
        if (live_stats && (now_us - last_stats_us >= 500000)) {
            print_stats();
            last_stats_us = now_us;
        }
    }
    uint64_t elapsed_us = to_us_since_boot(get_absolute_time()) - start_us;
    double secs = elapsed_us / 1e6;
    double kaccess = (double)accesses / 1000.0;
    double kacc_per_sec = kaccess / secs;
    if (out_accesses_k) *out_accesses_k = kaccess;
    if (out_secs) *out_secs = secs;
    free(buf);
    
    snprintf(log_buf, sizeof(log_buf), "[bench:rand_access] END accesses=%llu Kacc=%.1f time=%.3fs rate=%.1f Kacc/s freq=%uMHz temp=%.1f°C",
        (unsigned long long)accesses, kaccess, secs, kacc_per_sec, current_khz/1000, read_onboard_temperature());
    dmesg_log(log_buf);
    printf("%s\n", log_buf);
}

/* Helper: run a single target and optionally produce a CSV summary */
int bench_run_collect(const char *target, uint32_t ms, char *out, size_t out_len)
{
    char log_buf[128];
    if (!target) return -1;
    
    if (strcmp(target, "cpu") == 0) {
        uint64_t iters; double secs;
        measure_cpu(ms, &iters, &secs);
        if (out) snprintf(out, out_len, "%s,cpu,iterations,%llu,sec,%.3f", governors_get_current() ? governors_get_current()->name : "unknown", (unsigned long long)iters, secs);
        return 0;
    }
    if (strcmp(target, "memcpy") == 0) {
        double mb, secs;
        measure_memcpy(ms, &mb, &secs);
        if (out) snprintf(out, out_len, "%s,memcpy,MB,%.2f,sec,%.3f", governors_get_current() ? governors_get_current()->name : "unknown", mb, secs);
        return 0;
    }
    if (strcmp(target, "mem_stream_dma") == 0) {
        double mb, secs;
        measure_mem_stream_dma(ms, &mb, &secs);
        if (out) snprintf(out, out_len, "%s,mem_stream_dma,MB,%.2f,sec,%.3f", governors_get_current() ? governors_get_current()->name : "unknown", mb, secs);
        return 0;
    }
    if (strcmp(target, "memset") == 0) {
        double mb, secs;
        measure_memset(ms, &mb, &secs);
        if (out) snprintf(out, out_len, "%s,memset,MB,%.2f,sec,%.3f", governors_get_current() ? governors_get_current()->name : "unknown", mb, secs);
        return 0;
    }
    if (strcmp(target, "mem_stream") == 0) {
        double mb, secs;
        measure_mem_stream(ms, &mb, &secs);
        if (out) snprintf(out, out_len, "%s,mem_stream,MB,%.2f,sec,%.3f", governors_get_current() ? governors_get_current()->name : "unknown", mb, secs);
        return 0;
    }
    if (strcmp(target, "rand_access") == 0) {
        double kacc, secs;
        measure_rand_access(ms, &kacc, &secs);
        if (out) snprintf(out, out_len, "%s,rand_access,Kaccess,%.0f,sec,%.3f", governors_get_current() ? governors_get_current()->name : "unknown", kacc, secs);
        return 0;
    }
    return -1;
}


int bench_run(const char *target, uint32_t ms)
{
    char log_buf[192];
    char csv[192];
    const Governor *gov = governors_get_current();
    snprintf(log_buf, sizeof(log_buf), ">>> Running benchmark %s for %ums with governor: %s", target, ms, gov ? gov->name : "unknown");
    dmesg_log(log_buf);
    printf("%s\n", log_buf);
    
    if (bench_run_collect(target, ms, csv, sizeof(csv)) == 0) {
        /* Also print human-readable line for interactive use */
        printf("%s\n", csv);
        dmesg_log(csv);
        snprintf(log_buf, sizeof(log_buf), "<<< Benchmark %s completed. Results logged above.", target);
        dmesg_log(log_buf);
        printf("%s\n", log_buf);
        return 0;
    }
    snprintf(log_buf, sizeof(log_buf), "!!! Benchmark %s FAILED (unknown target)", target);
    dmesg_log(log_buf);
    printf("%s\n", log_buf);
    return -1;
}

void bench_suite(uint32_t ms_per_test, int csv)
{
    char log_buf[192];
    size_t n = governors_count();
    const char *targets[] = { "cpu", "memcpy", "memset", "mem_stream", "rand_access" };
    size_t tcount = sizeof(targets)/sizeof(targets[0]);

    snprintf(log_buf, sizeof(log_buf), "========== BENCHMARK SUITE START: %u ms per test, %zu governors, %zu benchmarks ==========", ms_per_test, n, tcount);
    dmesg_log(log_buf);
    printf("%s\n", log_buf);
    
    for (size_t i = 0; i < n; ++i) {
        const Governor *g = governors_get(i);
        if (!g) continue;
        
        snprintf(log_buf, sizeof(log_buf), "--- Switching to governor: %s", g->name);
        dmesg_log(log_buf);
        printf("%s\n", log_buf);
        
        governors_set_current(g);
        /* allow governor to settle */
        sleep_ms(250);
        
        for (size_t t = 0; t < tcount; ++t) {
            char out[256];
            if (bench_run_collect(targets[t], ms_per_test, out, sizeof(out)) == 0) {
                if (csv) {
                    /* CSV: governor,benchmark,metric,value,unit,sec,secs */
                    printf("%s\n", out);
                } else {
                    printf("%s\n", out);
                }
            }
            sleep_ms(20);
        }
        
        snprintf(log_buf, sizeof(log_buf), "--- Governor %s: all benchmarks complete", g->name);
        dmesg_log(log_buf);
        printf("%s\n", log_buf);
    }
    
    snprintf(log_buf, sizeof(log_buf), "========== BENCHMARK SUITE END ==========");
    dmesg_log(log_buf);
    printf("%s\n", log_buf);
}
