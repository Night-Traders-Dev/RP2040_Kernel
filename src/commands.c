#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "hardware/flash.h"
#include "hardware/clocks.h"
#include "commands.h"
#include "system.h"
#include "dmesg.h"
#include "governors.h"
#include "benchmark.h"
#include "metrics.h"
#include "uart_log.h"
#include "governors_rp2040_perf.h"
#include "persist.h"
#include "pio_idle.h"

/* Safe MMIO address range for peek/poke. */
#define SAFE_ADDR_MIN      0x10000000UL
#define SAFE_ADDR_MAX      0x50200000UL

static void cmd_set(const char *args)
{
    if (!args || !*args) { printf("Usage: set <mhz>\n"); return; }
    uint32_t mhz = (uint32_t)atoi(args);
    uint32_t khz = mhz * 1000;

    if (khz < MIN_KHZ || khz > MAX_KHZ) {
        printf("Out of range (%u - %u MHz)\n", MIN_KHZ / 1000, MAX_KHZ / 1000);
        return;
    }
    if (throttle_active) {
        printf("Warning: thermal throttle active. Target stored but may be overridden.\n");
    }
    target_khz = khz;
    printf("Target set to %u MHz\n", mhz);
}

static void cmd_peek(const char *args)
{
    if (!args || !*args) { printf("Usage: peek <hex_addr>\n"); return; }
    uint32_t addr = (uint32_t)strtoul(args, NULL, 16);

    if (addr % 4 != 0) {
        printf("Error: Address must be 32-bit aligned.\n");
        return;
    }
    if (addr < SAFE_ADDR_MIN || addr > SAFE_ADDR_MAX) {
        printf("Error: Address 0x%08lX is outside safe range "
               "(0x%08lX - 0x%08lX).\n",
               (unsigned long)addr,
               (unsigned long)SAFE_ADDR_MIN,
               (unsigned long)SAFE_ADDR_MAX);
        return;
    }

    volatile uint32_t *ptr = (volatile uint32_t *)addr;
    printf("[0x%08lX] = 0x%08lX\n", (unsigned long)addr, (unsigned long)*ptr);
}

static void cmd_poke(const char *args)
{
    if (!args || !*args) { printf("Usage: poke <hex_addr> <hex_value>\n"); return; }

    char *end;
    uint32_t addr  = (uint32_t)strtoul(args, &end, 16);
    uint32_t value = (uint32_t)strtoul(end, NULL, 16);

    if (addr % 4 != 0) {
        printf("Error: Address must be 32-bit aligned.\n");
        return;
    }
    if (addr < SAFE_ADDR_MIN || addr > SAFE_ADDR_MAX) {
        printf("Error: Address 0x%08lX is outside safe range.\n",
               (unsigned long)addr);
        return;
    }

    volatile uint32_t *ptr = (volatile uint32_t *)addr;
    *ptr = value;
    printf("[0x%08lX] <- 0x%08lX (readback: 0x%08lX)\n",
           (unsigned long)addr,
           (unsigned long)value,
           (unsigned long)*ptr);
}

static void cmd_clocks(const char *args)
{
    (void)args;
    printf("clk_sys  : %.3f MHz\n", clock_get_hz(clk_sys)  / 1e6f);
    printf("clk_peri : %.3f MHz\n", clock_get_hz(clk_peri) / 1e6f);
    printf("clk_usb  : %.3f MHz\n", clock_get_hz(clk_usb)  / 1e6f);
    printf("clk_adc  : %.3f MHz\n", clock_get_hz(clk_adc)  / 1e6f);
    printf("clk_rtc  : %.3f kHz\n", clock_get_hz(clk_rtc)  / 1e3f);
    printf("vreg     : %s\n", voltage_label(current_voltage_mv));
}

static void cmd_flash(const char *args)
{
    (void)args;
    printf("Flash size  : %u KB\n", PICO_FLASH_SIZE_BYTES / 1024);
    extern char __flash_binary_end;
    uint32_t fw_end   = (uint32_t)&__flash_binary_end;
    uint32_t fw_start = 0x10000000UL;
    uint32_t fw_used  = fw_end - fw_start;

    printf("Firmware    : %lu bytes (%.1f KB)\n",
           (unsigned long)fw_used,
           fw_used / 1024.0f);
    printf("Remaining   : %lu bytes (%.1f KB)\n",
           (unsigned long)(PICO_FLASH_SIZE_BYTES - fw_used),
           (PICO_FLASH_SIZE_BYTES - fw_used) / 1024.0f);
}

static void cmd_stats(const char *args)
{
    (void)args;
    live_stats = !live_stats;
    printf("Live stats %s\n", live_stats ? "enabled" : "disabled");
}

static void cmd_temp(const char *args)
{
    (void)args;
    printf("Core Temperature : %.1f °C\n", read_onboard_temperature());
    printf("Vreg             : %s\n", voltage_label(current_voltage_mv));
    printf("Throttle active  : %s\n", throttle_active ? "YES" : "no");
}

static void cmd_metrics(const char *args)
{
    (void)args;
    metrics_init();
    metrics_agg_t agg;
    uint32_t n = metrics_get_aggregate(&agg, 0);
    if (n == 0) {
        printf("No metrics samples available\n");
        return;
    }
    printf("Metrics samples: %u\n", n);
    printf("  avg workload : %.2f\n", agg.avg_workload);
    printf("  avg intensity: %.2f\n", agg.avg_intensity);
    printf("  avg duration : %.2f ms\n", agg.avg_duration_ms);
    printf("  last sample at: %u ms since boot\n", agg.last_ts_ms);

    kernel_metrics_t ks;
    if (metrics_get_kernel_snapshot(&ks)) {
        printf("Kernel snapshot:\n");
        printf("  gov tick count : %u\n", ks.gov_tick_count);
        printf("  gov tick avg   : %.3f ms\n", ks.gov_tick_avg_ms);
        printf("  last at        : %u ms since boot\n", ks.last_ts_ms);
    } else {
        printf("No kernel snapshot available\n");
    }

    size_t gov_n = governors_count();
    for (size_t i = 0; i < gov_n; ++i) {
        const Governor *g = governors_get(i);
        if (!g) continue;
        char s[128];
        if (g->export_stats) {
            g->export_stats(s, sizeof(s));
            printf("%s\n", s);
        }
    }
}

static void cmd_persist(const char *args)
{
    (void)args;
    char name[64];
    if (persist_load(name, sizeof(name)) == 0) {
        printf("Persisted governor: %s\n", name);
    } else {
        printf("No persisted governor found\n");
    }

    struct { char _pad[4]; } buf;
    if (persist_load_rp_params(&buf, sizeof(buf)) > 0) {
        printf("rp2040_perf parameters: present in flash\n");
    } else {
        printf("rp2040_perf parameters: not found\n");
    }
}

static void cmd_uptime(const char *args)
{
    (void)args;
    uint64_t us      = to_us_since_boot(get_absolute_time());
    uint32_t total_s = (uint32_t)(us / 1000000ULL);
    uint32_t h       = total_s / 3600;
    uint32_t m       = (total_s % 3600) / 60;
    uint32_t s       = total_s % 60;
    printf("Uptime: %02u:%02u:%02u\n", h, m, s);
}

static void cmd_dmesg(const char *args)
{
    if (!args || !*args) {
        dmesg_print();
        return;
    }

    char buf[64];
    strncpy(buf, args, sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';
    char *tok = strtok(buf, " ");
    if (tok && strcmp(tok, "uart") == 0) {
        char *opt = strtok(NULL, " ");
        if (!opt) { printf("Usage: dmesg uart <on|off>\n"); return; }
        if (strcmp(opt, "on") == 0) { uart_log_enable(1); printf("dmesg uart enabled\n"); }
        else if (strcmp(opt, "off") == 0) { uart_log_enable(0); printf("dmesg uart disabled\n"); }
        else printf("Usage: dmesg uart <on|off>\n");
        return;
    }
    dmesg_print();
}

static void cmd_bootsel(const char *args)
{
    (void)args;
    printf("Rebooting to BOOTSEL mode...\n");
    sleep_ms(100);
    reset_usb_boot(0, 0);
}

static void cmd_reboot(const char *args)
{
    (void)args;
    printf("Rebooting...\n");
    sleep_ms(100);
    watchdog_reboot(0, 0, 0);
}

static void cmd_gov(const char *args)
{
    if (!args || !*args) {
        printf("Usage: gov <list|set <name>|status>\n");
        return;
    }

    char buf[64];
    strncpy(buf, args, sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';

    char *cmd = strtok(buf, " ");
    if (!cmd) { printf("Usage: gov <list|set <name>|status>\n"); return; }

    if (strcmp(cmd, "list") == 0) {
        size_t n = governors_count();
        printf("Available governors:\n");
        for (size_t i = 0; i < n; ++i) {
            const Governor *g = governors_get(i);
            if (!g) continue;
            const Governor *cur = governors_get_current();
            printf("  %s %s\n", g->name, (g == cur) ? "(current)" : "");
        }
        return;
    }

    if (strcmp(cmd, "status") == 0) {
        const Governor *cur = governors_get_current();
        if (cur) printf("Current governor: %s\n", cur->name);
        else printf("No governor selected\n");
        return;
    }

    if (strcmp(cmd, "set") == 0) {
        char *name = strtok(NULL, " ");
        if (!name) { printf("Usage: gov set <name>\n"); return; }
        const Governor *g = governors_find_by_name(name);
        if (!g) { printf("Unknown governor: %s\n", name); return; }
        governors_set_current(g);
        printf("Governor set to %s\n", g->name);
        return;
    }

    if (strcmp(cmd, "tune") == 0) {
        char *name = strtok(NULL, " ");
        if (!name) { printf("Usage: gov tune <name> <show|get|set> [param] [value]\n"); return; }
        if (strcmp(name, "rp2040_perf") == 0) {
            char *sub = strtok(NULL, " ");
            if (!sub) { printf("Usage: gov tune rp2040_perf <show|get|set> [param] [value]\n"); return; }
            if (strcmp(sub, "show") == 0) { rp2040_perf_print_params(); return; }
            if (strcmp(sub, "get") == 0) {
                char *param = strtok(NULL, " ");
                if (!param) { printf("Usage: gov tune rp2040_perf get <param>\n"); return; }
                double v; if (rp2040_perf_get_param(param, &v) == 0) printf("%s = %.3f\n", param, v);
                else printf("Unknown param: %s\n", param);
                return;
            }
            if (strcmp(sub, "set") == 0) {
                char *param = strtok(NULL, " ");
                char *val_s = strtok(NULL, " ");
                if (!param || !val_s) { printf("Usage: gov tune rp2040_perf set <param> <value>\n"); return; }
                double v = atof(val_s);
                int rc = rp2040_perf_set_param(param, v);
                if (rc == 0) printf("Set %s = %.3f\n", param, v);
                else if (rc == -2) printf("Invalid value for %s: %s\n", param, val_s);
                else printf("Unknown param: %s\n", param);
                return;
            }
            if (strcmp(sub, "list") == 0) { rp2040_perf_list_params(); return; }
            printf("Unknown subcommand. Use show/get/set.\n");
            return;
        }
        printf("Unknown governor: %s\n", name);
        return;
    }

    printf("Unknown gov command. Use list/set/status.\n");
}

/* =========================================================================
 * PIO command group
 *
 *   pio                – print full stats snapshot (idle, jitter, stability)
 *   pio stats          – alias for bare `pio`
 *   pio safe           – one-shot safety gate query with verbose output
 *   pio reset          – reset jitter window (as if a freq change just occurred)
 *   pio watch <n>      – poll and print stats every <n> ms, n times (default 10×500ms)
 * ========================================================================= */

/* Pretty-print the full PIO stats snapshot. */
static void pio_print_stats(const pio_idle_stats_t *s, uint32_t sys_khz)
{
    /* Convert heartbeat period ticks → µs for readability */
    float hb_us = pio_idle_ticks_to_us(s->hb_period_ticks, sys_khz);

    printf("PIO Idle Monitor:\n");
    printf("  IDLE_PIN          : GPIO %d\n",   PIO_IDLE_PIN);
    printf("  HB_PIN            : GPIO %d\n",   PIO_HB_PIN);
    printf("  idle_ticks        : %lu\n",        (unsigned long)s->idle_ticks);
    printf("  idle_fraction     : %.1f %%\n",    s->idle_fraction * 100.0f);
    printf("  hb_period_ticks   : %lu  (%.2f us @ %u MHz)\n",
           (unsigned long)s->hb_period_ticks,
           hb_us,
           sys_khz / 1000u);
    printf("  hb_jitter_ticks   : %+ld\n",       (long)s->hb_jitter_ticks);
    printf("  hb_jitter_pct     : %.2f %%\n",    s->hb_jitter_pct);
    printf("  stable_count      : %lu\n",        (unsigned long)s->stable_count);
    printf("  safe_to_scale     : %s\n",         s->safe_to_scale ? "YES" : "no");
}

static void cmd_pio(const char *args)
{
    /* Resolve optional subcommand */
    char sub[32] = "";
    char rest[64] = "";

    if (args && *args) {
        /* split first token */
        const char *sp = args;
        while (*sp && *sp != ' ') sp++;
        size_t tok_len = (size_t)(sp - args);
        if (tok_len >= sizeof(sub)) tok_len = sizeof(sub) - 1;
        memcpy(sub, args, tok_len);
        sub[tok_len] = '\0';
        if (*sp == ' ') {
            strncpy(rest, sp + 1, sizeof(rest) - 1);
            rest[sizeof(rest) - 1] = '\0';
        }
    }

    /* ---- bare `pio` or `pio stats` ---- */
    if (sub[0] == '\0' || strcmp(sub, "stats") == 0) {
        pio_idle_stats_t s;
        pio_idle_get_stats(&s);
        pio_print_stats(&s, current_khz);
        return;
    }

    /* ---- `pio safe` ---- */
    if (strcmp(sub, "safe") == 0) {
        pio_idle_stats_t s;
        pio_idle_get_stats(&s);

        /* Use the same defaults as rp2040_perf governor */
        const float  idle_thresh   = 0.03f;
        const float  jitter_thresh = 3.0f;
        const uint32_t min_stable  = 4u;

        bool safe = pio_idle_safe_to_scale(idle_thresh, jitter_thresh, min_stable);

        printf("PIO Safety Gate:\n");
        printf("  idle_thresh    : %.0f %%  (%.1f %%)\n",
               idle_thresh * 100.0f, s.idle_fraction * 100.0f);
        printf("  jitter_thresh  : %.1f %%  (%.2f %%)\n",
               jitter_thresh, s.hb_jitter_pct);
        printf("  min_stable     : %u     (%lu seen)\n",
               min_stable, (unsigned long)s.stable_count);
        printf("  safe_to_scale  : %s\n", safe ? "YES" : "NO");

        if (!safe) {
            /* Give operator a hint about what is blocking the gate */
            if (s.stable_count < min_stable)
                printf("  [reason] stable_count %lu < %u — waiting for more stable HB readings\n",
                       (unsigned long)s.stable_count, min_stable);
            if (s.hb_jitter_pct > jitter_thresh)
                printf("  [reason] jitter %.2f %% > %.1f %% threshold\n",
                       s.hb_jitter_pct, jitter_thresh);
        }
        return;
    }

    /* ---- `pio reset` ---- */
    if (strcmp(sub, "reset") == 0) {
        pio_idle_notify_freq_change(current_khz);
        printf("PIO jitter window reset (settle started @ %u kHz).\n", current_khz);
        dmesg_log("cmd:pio reset — jitter window cleared by user");
        return;
    }

    /* ---- `pio watch [interval_ms [count]]` ---- */
    if (strcmp(sub, "watch") == 0) {
        /* Parse optional interval_ms and count from rest */
        uint32_t interval_ms = 500;
        uint32_t count       = 10;

        char *tok = strtok(rest, " ");
        if (tok) {
            int v = atoi(tok);
            if (v > 0) interval_ms = (uint32_t)v;
            tok = strtok(NULL, " ");
            if (tok) {
                v = atoi(tok);
                if (v > 0) count = (uint32_t)v;
            }
        }

        printf("Watching PIO stats every %u ms, %u samples "
               "(press any key to abort):\n\n",
               interval_ms, count);

        for (uint32_t i = 0; i < count; ++i) {
            /* Check for keypress abort */
            int c = getchar_timeout_us(0);
            if (c != PICO_ERROR_TIMEOUT) {
                printf("\nAborted.\n");
                return;
            }

            pio_idle_stats_t s;
            pio_idle_get_stats(&s);

            printf("[%2u/%2u] idle=%.1f%%  jitter=%+.2f%%  stable=%lu  %s\n",
                   i + 1, count,
                   s.idle_fraction * 100.0f,
                   s.hb_jitter_pct,
                   (unsigned long)s.stable_count,
                   s.safe_to_scale ? "SAFE" : "wait");

            sleep_ms(interval_ms);
        }
        printf("\nWatch complete.\n");
        return;
    }

    /* ---- unknown subcommand ---- */
    printf("Usage:\n"
           "  pio               Show full PIO stats snapshot\n"
           "  pio stats         Alias for bare 'pio'\n"
           "  pio safe          Verbose safety gate query\n"
           "  pio reset         Reset jitter window (simulate freq change)\n"
           "  pio watch [ms [n]] Poll stats every <ms> ms, <n> times\n");
}

static void cmd_help(const char *args); /* forward decl */

typedef struct {
    const char *name;
    void        (*fn)(const char *args);
    const char  *usage;
    const char  *desc;
} Command;

static void cmd_clear(const char *args)
{
    (void)args;
    printf("\x1b[2J\x1b[H");
}

static void cmd_bench(const char *args)
{
    if (!args || !*args) {
        printf("Usage: bench <target> <ms>\n");
        bench_list();
        return;
    }

    char buf[64];
    strncpy(buf, args, sizeof(buf)-1);
    buf[sizeof(buf)-1] = '\0';
    char *tok = strtok(buf, " ");
    if (!tok) { printf("Usage: bench cpu <ms>\n"); return; }

    if (strcmp(tok, "cpu") == 0) {
        char *dur_s = strtok(NULL, " ");
        uint32_t ms = 1000;
        if (dur_s) ms = (uint32_t)atoi(dur_s);
        if (bench_run("cpu", ms) == 0) return;
    }

    if (strcmp(tok, "suite") == 0) {
        char *dur_s = strtok(NULL, " ");
        uint32_t ms = 1000;
        if (dur_s) ms = (uint32_t)atoi(dur_s);
        char *opt = strtok(NULL, " ");
        int csv = 0;
        if (opt && strcmp(opt, "csv") == 0) csv = 1;
        bench_suite(ms, csv);
        return;
    }

    char *dur_s = strtok(NULL, " ");
    uint32_t ms = 1000;
    if (dur_s) ms = (uint32_t)atoi(dur_s);
    if (bench_run(tok, ms) == 0) return;

    printf("Unknown bench target '%s'. Supported: \n", tok);
    bench_list();
}

static const Command commands[] = {
    { "set",     cmd_set,     "set <mhz>",                    "Set target frequency (125-264 MHz)"            },
    { "peek",    cmd_peek,    "peek <hex>",                   "Read 32-bit MMIO register"                     },
    { "poke",    cmd_poke,    "poke <hex> <hex>",             "Write 32-bit value to MMIO register"           },
    { "clocks",  cmd_clocks,  "clocks",                       "Dump all PLL/clock divider frequencies"        },
    { "flash",   cmd_flash,   "flash",                        "Show flash size and firmware usage"            },
    { "stats",   cmd_stats,   "stats",                        "Toggle live clock/temp display"                },
    { "temp",    cmd_temp,    "temp",                         "Read core temperature and vreg state"          },
    { "uptime",  cmd_uptime,  "uptime",                       "Show system uptime"                            },
    { "dmesg",   cmd_dmesg,   "dmesg",                        "Print system log"                              },
    { "bootsel", cmd_bootsel, "bootsel",                      "Reboot into UF2 flash mode"                    },
    { "reboot",  cmd_reboot,  "reboot",                       "Restart system"                               },
    { "metrics", cmd_metrics, "metrics",                      "Show aggregated app-submitted metrics"         },
    { "persist", cmd_persist, "persist",                      "Show persisted governor and rp_params status"  },
    { "pio",     cmd_pio,     "pio [stats|safe|reset|watch]", "PIO idle/jitter subsystem commands"            },
    { "help",    cmd_help,    "help",                         "Show this help"                                },
    { "gov",     cmd_gov,     "gov <list|set|status>",        "Governor controls (list/set/status)"           },
    { "clear",   cmd_clear,   "clear",                        "Clear the screen"                              },
    { "bench",   cmd_bench,   "bench <target> <ms>",          "Run benchmark on specified target"             },
};

#define NUM_COMMANDS (sizeof(commands) / sizeof(commands[0]))

static void cmd_help(const char *args)
{
    (void)args;
    printf("\nRP2040 Minishell Commands:\n");
    printf("%-34s %s\n", "Usage", "Description");
    printf("%-34s %s\n", "-----", "-----------");
    for (size_t i = 0; i < NUM_COMMANDS; i++)
        printf("  %-32s %s\n", commands[i].usage, commands[i].desc);
    printf("\n");
    printf("PIO subcommands:\n");
    printf("  %-32s %s\n", "pio",              "Full stats snapshot");
    printf("  %-32s %s\n", "pio stats",        "Alias for bare 'pio'");
    printf("  %-32s %s\n", "pio safe",         "Verbose safety gate query");
    printf("  %-32s %s\n", "pio reset",        "Reset jitter window");
    printf("  %-32s %s\n", "pio watch [ms [n]]","Poll stats every <ms> ms, <n> times");
    printf("\n");
}

void dispatch(const char *input)
{
    if (!input || input[0] == '\0') return;

    for (size_t i = 0; i < NUM_COMMANDS; i++) {
        size_t nlen = strlen(commands[i].name);
        if (strncmp(input, commands[i].name, nlen) == 0 &&
            (input[nlen] == '\0' || input[nlen] == ' '))
        {
            const char *args = (input[nlen] == ' ') ? &input[nlen + 1] : NULL;
            commands[i].fn(args);
            return;
        }
    }

    printf("Unknown command: '%s'. Type 'help' for a list.\n", input);
}