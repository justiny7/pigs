#include "lib.h"
#include "uart.h"

#define PM_RSTC 0x2010001C
#define PM_WDOG 0x20100024
#define PM_PASSWORD 0x5A000000
#define PM_RSTC_WRCFG_FULL_RESET 0x20

void rpi_reboot() {
    uart_flush_tx();

    PUT32(PM_WDOG, PM_PASSWORD | 1);
    PUT32(PM_RSTC, PM_PASSWORD | PM_RSTC_WRCFG_FULL_RESET);
}
void rpi_reset() {
    uart_putk("\r\nDONE!!!\n");
    rpi_reboot();
}

void assert(bool val, const char* msg) {
    if (!val) {
        uart_puts("\n[ERROR] Assertion failed: ");
        uart_puts(msg);
        rpi_reset();
    }
}
void panic(const char* msg) {
    uart_puts("\n[PANIC] ");
    uart_puts(msg);
    rpi_reset();
}

void* memcpy(void* dst, const void* src, uint32_t n) {
    uint8_t* d = (uint8_t*) dst;
    const uint8_t* s = (const uint8_t*) src;
    while (n--) *d++ = *s++;
    return dst;
}
void* memset(void* dst, int val, uint32_t n) {
    uint8_t* d = (uint8_t*) dst;
    while (n--) *d++ = (uint8_t) val;
    return dst;
}

void caches_enable() {
    uint32_t r;
    asm volatile ("MRC p15, 0, %0, c1, c0, 0" : "=r" (r));
    r |= (1 << 12); // l1 instruction cache
    r |= (1 << 11); // branch prediction
    r |= (1 << 2); // data cache
    asm volatile ("MCR p15, 0, %0, c1, c0, 0" :: "r" (r));
}

void caches_disable() {
    uint32_t r;
    asm volatile ("MRC p15, 0, %0, c1, c0, 0" : "=r" (r));
    r &= ~(1 << 12); // l1 instruction cache
    r &= ~(1 << 11); // branch prediction
    r &= ~(1 << 2); // data cache
    asm volatile ("MCR p15, 0, %0, c1, c0, 0" :: "r" (r));
}

int errno;
int* __errno() {
    return &errno;
}
