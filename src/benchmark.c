#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "benchmark.h"
#include "dmesg.h"
#include "governors.h"

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

/* Measurement helpers return primary metric and seconds */
static void measure_cpu(uint32_t ms, uint64_t *out_iters, double *out_secs)
{
    uint64_t start_us = to_us_since_boot(get_absolute_time());
    uint64_t end_us = start_us + (uint64_t)ms * 1000ULL;
    volatile uint32_t acc = 0;
    uint64_t iter = 0;
    while (to_us_since_boot(get_absolute_time()) < end_us) {
        acc += (uint32_t)(iter ^ (iter << 1));
        iter++;
    }
    uint64_t elapsed_us = to_us_since_boot(get_absolute_time()) - start_us;
    double secs = elapsed_us / 1e6;
    (void)acc;
    if (out_iters) *out_iters = iter;
    if (out_secs) *out_secs = secs;
}

/* Memory throughput helpers: use moderate buffer sizes to fit RP2040 RAM */
#define BUF_SIZE (32 * 1024)

static void measure_memcpy(uint32_t ms, double *out_mb, double *out_secs)
{
    uint8_t *src = malloc(BUF_SIZE);
    uint8_t *dst = malloc(BUF_SIZE);
    if (!src || !dst) { if (out_mb) *out_mb = 0.0; if (out_secs) *out_secs = 0.0; free(src); free(dst); return; }
    for (size_t i = 0; i < BUF_SIZE; ++i) src[i] = (uint8_t)i;
    uint64_t start_us = to_us_since_boot(get_absolute_time());
    uint64_t end_us = start_us + (uint64_t)ms * 1000ULL;
    uint64_t ops = 0;
    while (to_us_since_boot(get_absolute_time()) < end_us) {
        memcpy(dst, src, BUF_SIZE);
        ops++;
    }
    uint64_t elapsed_us = to_us_since_boot(get_absolute_time()) - start_us;
    double secs = elapsed_us / 1e6;
    double mb = (double)(ops * BUF_SIZE) / (1024.0 * 1024.0);
    if (out_mb) *out_mb = mb;
    if (out_secs) *out_secs = secs;
    free(src); free(dst);
}

static void measure_memset(uint32_t ms, double *out_mb, double *out_secs)
{
    uint8_t *buf = malloc(BUF_SIZE);
    if (!buf) { if (out_mb) *out_mb = 0.0; if (out_secs) *out_secs = 0.0; return; }
    uint64_t start_us = to_us_since_boot(get_absolute_time());
    uint64_t end_us = start_us + (uint64_t)ms * 1000ULL;
    uint64_t ops = 0;
    while (to_us_since_boot(get_absolute_time()) < end_us) {
        memset(buf, 0xA5, BUF_SIZE);
        ops++;
    }
    uint64_t elapsed_us = to_us_since_boot(get_absolute_time()) - start_us;
    double secs = elapsed_us / 1e6;
    double mb = (double)(ops * BUF_SIZE) / (1024.0 * 1024.0);
    if (out_mb) *out_mb = mb;
    if (out_secs) *out_secs = secs;
    free(buf);
}

static void measure_mem_stream(uint32_t ms, double *out_mb, double *out_secs)
{
    uint8_t *buf = malloc(BUF_SIZE);
    if (!buf) { if (out_mb) *out_mb = 0.0; if (out_secs) *out_secs = 0.0; return; }
    for (size_t i = 0; i < BUF_SIZE; ++i) buf[i] = (uint8_t)(i & 0xFF);
    uint64_t start_us = to_us_since_boot(get_absolute_time());
    uint64_t end_us = start_us + (uint64_t)ms * 1000ULL;
    uint64_t bytes = 0;
    while (to_us_since_boot(get_absolute_time()) < end_us) {
        for (size_t i = 0; i < BUF_SIZE; ++i) {
            volatile uint8_t v = buf[i]; (void)v;
        }
        bytes += BUF_SIZE;
    }
    uint64_t elapsed_us = to_us_since_boot(get_absolute_time()) - start_us;
    double secs = elapsed_us / 1e6;
    double mb = (double)bytes / (1024.0 * 1024.0);
    if (out_mb) *out_mb = mb;
    if (out_secs) *out_secs = secs;
    free(buf);
}

/* DMA-backed memory stream: repeatedly DMA-copy a buffer to a second buffer
 * measuring throughput while the CPU remains mostly idle between transfers.
 */
#include "hardware/dma.h"

static void measure_mem_stream_dma(uint32_t ms, double *out_mb, double *out_secs)
{
    uint8_t *src = malloc(BUF_SIZE);
    uint8_t *dst = malloc(BUF_SIZE);
    if (!src || !dst) { if (out_mb) *out_mb = 0.0; if (out_secs) *out_secs = 0.0; free(src); free(dst); return; }
    for (size_t i = 0; i < BUF_SIZE; ++i) src[i] = (uint8_t)(i & 0xFF);

    int ch = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);

    uint64_t start_us = to_us_since_boot(get_absolute_time());
    uint64_t end_us = start_us + (uint64_t)ms * 1000ULL;
    uint64_t ops = 0;
    while (to_us_since_boot(get_absolute_time()) < end_us) {
        dma_channel_configure(ch, &c, dst, src, BUF_SIZE, true);
        dma_channel_wait_for_finish_blocking(ch);
        ops++;
    }
    uint64_t elapsed_us = to_us_since_boot(get_absolute_time()) - start_us;
    double secs = elapsed_us / 1e6;
    double mb = (double)(ops * BUF_SIZE) / (1024.0 * 1024.0);
    if (out_mb) *out_mb = mb;
    if (out_secs) *out_secs = secs;

    dma_channel_unclaim(ch);
    free(src); free(dst);
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
    uint8_t *buf = malloc(BUF_SIZE);
    if (!buf) { if (out_accesses_k) *out_accesses_k = 0.0; if (out_secs) *out_secs = 0.0; return; }
    for (size_t i = 0; i < BUF_SIZE; ++i) buf[i] = (uint8_t)(i & 0xFF);
    uint64_t start_us = to_us_since_boot(get_absolute_time());
    uint64_t end_us = start_us + (uint64_t)ms * 1000ULL;
    uint64_t accesses = 0;
    while (to_us_since_boot(get_absolute_time()) < end_us) {
        uint32_t idx = rng_next() % BUF_SIZE;
        volatile uint8_t v = buf[idx]; (void)v;
        accesses++;
    }
    uint64_t elapsed_us = to_us_since_boot(get_absolute_time()) - start_us;
    double secs = elapsed_us / 1e6;
    double kaccess = (double)accesses / 1000.0;
    if (out_accesses_k) *out_accesses_k = kaccess;
    if (out_secs) *out_secs = secs;
    free(buf);
}

/* Helper: run a single target and optionally produce a CSV summary */
int bench_run_collect(const char *target, uint32_t ms, char *out, size_t out_len)
{
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
    char csv[192];
    if (bench_run_collect(target, ms, csv, sizeof(csv)) == 0) {
        /* Also print human-readable line for interactive use */
        printf("%s\n", csv);
        dmesg_log(csv);
        return 0;
    }
    return -1;
}

void bench_suite(uint32_t ms_per_test, int csv)
{
    size_t n = governors_count();
    const char *targets[] = { "cpu", "memcpy", "memset", "mem_stream", "rand_access" };
    size_t tcount = sizeof(targets)/sizeof(targets[0]);

    printf("Running benchmark suite: %u ms per test across %zu governors\n", ms_per_test, n);
    for (size_t i = 0; i < n; ++i) {
        const Governor *g = governors_get(i);
        if (!g) continue;
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
    }
}
