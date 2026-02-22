#ifndef UART_LOG_H
#define UART_LOG_H

#include <stddef.h>

/* Initialize UART+DMA logging backend. Call once at startup. */
int uart_log_init(unsigned baud, int tx_pin);

/* Send a nul-terminated log message via DMA-backed UART. Returns 0 if queued,
 * or -1 if the DMA is busy and message was dropped. */
int uart_log_send(const char *msg);

/* Enable/disable UART logging at runtime */
void uart_log_enable(int en);
int uart_log_enabled(void);

#endif
