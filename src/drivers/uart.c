#include "lib.h"
#include "gpio.h"
#include "uart.h"
#include "mailbox_interface.h"

void uart_init() {
    mem_barrier_dsb();

    Pin tx = { UART_TX_PIN };
    Pin rx = { UART_RX_PIN };
    gpio_select(tx, GPIO_ALT_FUNC_5);
    gpio_select(rx, GPIO_ALT_FUNC_5);

    // Disable pulls
    gpio_set_pull(tx, GPIO_PULL_OFF);
    gpio_set_pull(rx, GPIO_PULL_OFF);

    mem_barrier_dsb();

    OR32(AUX_ENABLES, UART_ENABLE_BIT);

    mem_barrier_dsb();

    uart_disable();
    uart_disable_interrupts();

    // set 8-bit mode (write 0b11)
    PUT32(AUX_MU_LCR_REG, 3);

    // default high
    PUT32(AUX_MU_MCR_REG, 0);

    // clear FIFOs
    PUT32(AUX_MU_IIR_REG, 6);

    uart_set_baudrate(UART_BAUDRATE_PRESCALE);
    uart_enable();

    mem_barrier_dsb();
}

void uart_set_baudrate(uint32_t baudrate_reg) {
    PUT32(AUX_MU_BAUD_REG, baudrate_reg);
}
void uart_set_baudrate_to_clock(uint32_t clock_rate) {
    uint32_t baudrate_reg = clock_rate / (8 * UART_BAUDRATE) - 1;
    mem_barrier_dsb();
    PUT32(AUX_MU_BAUD_REG, baudrate_reg);
    mem_barrier_dsb();
}
void uart_set_baudrate_to_core_clock() {
    uart_set_baudrate_to_clock(mbox_get_measured_clock_rate(MBOX_CLK_CORE));
}

void uart_enable() {
    PUT32(AUX_MU_CNTL_REG, UART_RX_CNTL_BIT | UART_TX_CNTL_BIT);
}
void uart_rx_enable() {
    PUT32(AUX_MU_CNTL_REG, UART_RX_CNTL_BIT);
}
void uart_tx_enable() {
    PUT32(AUX_MU_CNTL_REG, UART_TX_CNTL_BIT);
}
void uart_disable() {
    PUT32(AUX_MU_CNTL_REG, 0);
}

void uart_disable_interrupts() {
    PUT32(AUX_MU_IER_REG, 0);
}

// helpers w/ no barriers
bool _uart_can_getc() {
    return GET32(AUX_MU_LSR_REG) & UART_RX_READY_BIT;
}
bool _uart_can_putc() {
    return GET32(AUX_MU_LSR_REG) & UART_TX_READY_BIT;
}
bool _uart_tx_is_empty() {
    return GET32(AUX_MU_LSR_REG) & UART_TX_EMPTY_BIT;
}
uint8_t _uart_getc() {
    while (!_uart_can_getc());
    return GET32(AUX_MU_IO_REG) & 0xFF;
}
void _uart_putc(uint8_t c) {
    while (!_uart_can_putc());
    PUT32(AUX_MU_IO_REG, (uint32_t) c);
}

uint8_t uart_getc() {
    mem_barrier_dsb();
    uint8_t res = _uart_getc();
    mem_barrier_dsb();

    return res;
}
void uart_putc(uint8_t c) {
    mem_barrier_dsb();
    _uart_putc(c);
    mem_barrier_dsb();
}
void uart_puts(const char* s) {
    mem_barrier_dsb();
    while (*s) {
        if (*s == '\n') {
            _uart_putc('\r');
        }
        _uart_putc(*s++);
    }
    mem_barrier_dsb();
}
void uart_putk(const char* s) {
    mem_barrier_dsb();
    while (*s) {
        _uart_putc(*s++);
    }
    mem_barrier_dsb();
}
void uart_putd(uint32_t x) {
    mem_barrier_dsb();
    if (x == 0) {
        _uart_putc('0');
        return;
    }

    uint8_t num[10] = {};
    int i = -1;
    for (; x > 0; x /= 10) {
        num[++i] = '0' + (x % 10);
    }
    while (i >= 0) {
        _uart_putc(num[i--]);
    }
    mem_barrier_dsb();
}
void uart_putx(uint32_t x) {
    mem_barrier_dsb();
    _uart_putc('0');
    _uart_putc('x');
    for (int i = 7 * 4; i >= 0; i -= 4) {
        uint32_t d = (x >> i) & 0xF;
        _uart_putc(d + ((d < 10) ? '0' : ('A' - 10)));
    }
    mem_barrier_dsb();
}
void uart_hex(uint32_t x) {
    uart_putx(x);
}
void uart_putb(uint32_t x) {
    mem_barrier_dsb();
    _uart_putc('0');
    _uart_putc('b');
    for (int i = 31; i >= 0; i--) {
        if ((1U << i) <= x) {
            _uart_putc('0' + ((x >> i) & 1));
        }
    }
    mem_barrier_dsb();
}
void uart_putf(float x) {
    uart_putf_precision(x, 3);
}
void uart_putf_precision(float x, uint32_t precision) {
    if (x < 0) {
        uart_putc('-');
        x = -x;
    }

    uint32_t whole = (uint32_t) x;
    uart_putd(whole);
    uart_putc('.');

    float frac = x - (float) whole;
    while (precision--) {
        frac *= 10;
        uint32_t d = (uint32_t) frac;
        uart_putc('0' + d);
        frac -= (float) d;
    }
}

bool uart_can_getc() {
    mem_barrier_dsb();
    bool res = _uart_can_getc();
    mem_barrier_dsb();
    return res;
}
bool uart_can_putc() {
    mem_barrier_dsb();
    bool res = _uart_can_putc();
    mem_barrier_dsb();
    return res;
}

bool uart_tx_is_empty() {
    mem_barrier_dsb();
    bool res = _uart_tx_is_empty();
    mem_barrier_dsb();
    return res;
}
void uart_flush_tx() {
    mem_barrier_dsb();
    while (!_uart_tx_is_empty());
    mem_barrier_dsb();
}
