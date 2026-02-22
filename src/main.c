/* RP2040 Minishell - Modularized main
 *
 * This file now initializes subsystems and runs the REPL loop.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/adc.h"
#include "hardware/watchdog.h"
#include "hardware/clocks.h"

#include "dmesg.h"
#include "system.h"
#include "commands.h"

int main(void)
{
    stdio_init_all();

    adc_init();
    adc_set_temp_sensor_enabled(true);

    /* Give USB time to enumerate */
    sleep_ms(3000);
    while (!stdio_usb_connected())
        sleep_ms(100);

    dmesg_init();

    printf("\n--- RP2040 Minishell (boot) ---\n");
    printf("Initial clock : %.2f MHz\n", clock_get_hz(clk_sys) / 1e6f);
    dmesg_log("System boot complete");

    multicore_lockout_victim_init();
    multicore_launch_core1(core1_entry);

    printf("Type 'help' for available commands.\n");
    printf("--- RP2040 Minishell Ready ---\n");

    char     input[64];
    int      idx = 0;

    uint32_t last_stat_ms  = to_ms_since_boot(get_absolute_time());
    uint32_t last_ping_val = 0;
    uint32_t last_ping_ms  = to_ms_since_boot(get_absolute_time());

    printf("\n> ");
    fflush(stdout);

    while (true) {
        int c = getchar_timeout_us(0);

        if (c == PICO_ERROR_TIMEOUT) {
            sleep_us(100);

            uint32_t now_ms = to_ms_since_boot(get_absolute_time());

            if (live_stats && (now_ms - last_stat_ms) >= 500) {
                print_stats();
                last_stat_ms = now_ms;
            }

            if ((now_ms - last_ping_ms) >= 5000) {
                if (core1_wdt_ping == last_ping_val) {
                    dmesg_log("CRITICAL: Core 1 watchdog timeout. Rebooting.");
                    printf("\nCRITICAL: Core 1 watchdog timeout. Rebooting...\n");
                    sleep_ms(200);
                    watchdog_reboot(0, 0, 0);
                }
                last_ping_val = core1_wdt_ping;
                last_ping_ms  = now_ms;
            }

            continue;
        }

        if (c == '\r' || c == '\n') {
            input[idx] = '\0';
            idx = 0;
            if (live_stats) printf("\n");
            printf("\n");
            dispatch(input);
            printf("\n> ");
            fflush(stdout);
            continue;
        }

        if ((c == 8 || c == 127) && idx > 0) {
            idx--;
            printf("\b \b");
            fflush(stdout);
            continue;
        }

        if (idx < (int)sizeof(input) - 1) {
            input[idx++] = (char)c;
            putchar(c);
            fflush(stdout);
        }
    }
}
