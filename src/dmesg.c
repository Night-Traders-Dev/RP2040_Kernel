#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/sync.h"
#include "dmesg.h"
#include "uart_log.h"

#define LOG_SIZE 64
#define LOG_LEN  96

static char    log_buffer[LOG_SIZE][LOG_LEN];
static int     log_head = 0;
static mutex_t log_mutex;

void dmesg_init(void)
{
    mutex_init(&log_mutex);
    uart_log_init(115200, 0);
}

void dmesg_log(const char *msg)
{
    mutex_enter_blocking(&log_mutex);
    snprintf(log_buffer[log_head], LOG_LEN, "%lu: %s",
             to_ms_since_boot(get_absolute_time()), msg);
    log_head = (log_head + 1) % LOG_SIZE;
    mutex_exit(&log_mutex);
    if (uart_log_enabled()) {
        uart_log_send(log_buffer[(log_head + LOG_SIZE - 1) % LOG_SIZE]);
    }
}

void dmesg_print(void)
{
    mutex_enter_blocking(&log_mutex);
    printf("\n--- DMESG ---\n");
    for (int i = 0; i < LOG_SIZE; i++) {
        int idx = (log_head + i) % LOG_SIZE;
        if (log_buffer[idx][0])
            printf("%s\n", log_buffer[idx]);
    }
    printf("-------------\n");
    mutex_exit(&log_mutex);
}
