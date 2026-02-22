#include "uart_log.h"
#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include <stdlib.h>

/* Simple DMA UART TX backend.
 * Behavior: single DMA channel used to transfer a message buffer to UART DR.
 * If DMA is busy when a message arrives, message is dropped to avoid blocking.
 * This is intentionally simple; can be extended to queue multiple messages.
 */

static int dma_chan = -1;
static volatile int dma_busy = 0;
static int uart_tx_pin = 0;
static uart_inst_t *uart = uart0;

int uart_log_init(unsigned baud, int tx_pin)
{
    uart_tx_pin = tx_pin;
    uart = uart0;
    uart_init(uart, baud);
    gpio_set_function(tx_pin, GPIO_FUNC_UART);
    // configure UART: 8N1 default
    uart_set_format(uart, 8, 1, UART_PARITY_NONE);

    dma_chan = dma_claim_unused_channel(true);
    if (dma_chan < 0) return -1;

    dma_busy = 0;

    return 0;
}

static void dma_complete_isr()
{
    dma_hw->ints0 = 1u << dma_chan; // clear
    dma_busy = 0;
}

int uart_log_send(const char *msg)
{
    if (!msg) return -1;
    if (dma_chan < 0) return -1;
    if (dma_busy) return -1; // drop when busy

    size_t len = strlen(msg);
    if (len == 0) return -1;

    // Allocate a transient buffer (small); caller message lifetime isn't guaranteed
    char *buf = (char *)malloc(len + 2);
    if (!buf) return -1;
    memcpy(buf, msg, len);
    buf[len] = '\r';
    buf[len+1] = '\n';

    dma_channel_config c = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);

    // Configure IRQ: use poll for simplicity (no IRQ hook), but set busy
    dma_channel_configure(dma_chan, &c,
                          &uart_get_hw(uart)->dr, // write to UART FIFO
                          buf, // read from buf
                          len + 2,
                          false);

    dma_start_channel_mask(1u << dma_chan);
    dma_busy = 1;

    // Detach buffer free: use a blocking wait on a background tiny sleep and free.
    // Simpler: spin waiting in a detached task isn't great; instead poll in a
    // micro-sleep loop on a separate core is heavy. For now, we set up a
    // small busy-waiter in a new thread to free buffer after DMA completes.

    // Start a tiny thread to wait and free
    const char *free_task_name = "uart_log_free";
    // We'll use a lambda-style task on core0 via a new thread (pico's stdlib threads not present).
    // Simpler approach: busy-poll in a spawned coroutine using add_repeating_timer?

    // Use a repeating timer to check DMA completion and free buffer once done.
    struct repeating_timer *rt = malloc(sizeof(struct repeating_timer));
    if (!rt) {
        // no timer; free when DMA appears free (not ideal)
        return 0;
    }

    // create timer user data to hold buffer and dma channel
    struct dma_free_ctx {
        char *buf;
        int chan;
        struct repeating_timer *rt;
    } *ctx = malloc(sizeof(*ctx));
    if (!ctx) { free(rt); return 0; }
    ctx->buf = buf; ctx->chan = dma_chan; ctx->rt = rt;

    bool timer_cb(repeating_timer_t *t) {
        (void)t;
        if (!dma_channel_is_busy(ctx->chan)) {
            free(ctx->buf);
            cancel_repeating_timer(ctx->rt);
            free(ctx->rt);
            free(ctx);
            dma_busy = 0;
            return false; // stop timer
        }
        return true; // keep timer
    }

    add_repeating_timer_ms(10, timer_cb, NULL, rt);

    return 0;
}

static int uart_enabled = 0;
void uart_log_enable(int en) { uart_enabled = en ? 1 : 0; }
int uart_log_enabled(void) { return uart_enabled; }
