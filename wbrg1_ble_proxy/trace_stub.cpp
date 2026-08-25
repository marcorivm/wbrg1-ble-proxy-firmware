// BT trace/snoop output disabled at the hardware boundary.
//
// Why: every board-2 connect freeze sampled the BT stack's trace path
// (log_snoop / trace_log_buffer / trace_print) interleaved with the LE
// connection-complete burst. trace_uart_tx arms the trace UART TX interrupt
// and then waits FOREVER on a semaphore that only traceuart_irq gives — a
// per-unit-timing hang waiting to happen, inside the BT stack's hot path.
// These strong definitions override the weakened symbols in lib_arduino.a:
// the trace UART hardware is never initialised and every trace buffer is
// "sent" instantly, so the stack's logging can never block anything.
#include <stdint.h>
#include <stdbool.h>
extern "C" {
typedef bool (*UART_TX_CB)(void);
bool trace_uart_init(void)   { return true; }
bool trace_uart_deinit(void) { return true; }
bool trace_uart_tx(uint8_t *p_buf, uint16_t len, UART_TX_CB tx_cb) {
    (void)p_buf; (void)len;
    if (tx_cb) tx_cb();
    return true;
}
}
