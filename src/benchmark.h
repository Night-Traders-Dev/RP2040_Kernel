#ifndef BENCHMARK_H
#define BENCHMARK_H

#include <stdint.h>

/* Print available benchmarks to stdout */
void bench_list(void);

/* Run a benchmark named `target` for `ms` milliseconds. Returns 0 on success, -1 if unknown. */
int bench_run(const char *target, uint32_t ms);

/* Run a full suite across all governors. ms_per_test is duration per benchmark.
 * If csv is non-zero, output CSV lines suitable for parsing.
 */
void bench_suite(uint32_t ms_per_test, int csv);

/* Run a benchmark but return a CSV summary in out (if not NULL). */
int bench_run_collect(const char *target, uint32_t ms, char *out, size_t out_len);

#endif
