#include "lib.h"
#include "uart.h"
#include "arena_allocator.h"

#define PM_RSTC 0x2010001C
#define PM_WDOG 0x20100024
#define PM_PASSWORD 0x5A000000
#define PM_RSTC_WRCFG_FULL_RESET 0x20

static Arena heap_allocator;
extern uint8_t __heap_start__[];

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
    // can't use 32-bit copies bc src and dst might not be the same alignment
    uint8_t* d = (uint8_t*) dst;
    const uint8_t* s = (const uint8_t*) src;
    while (n--) *d++ = *s++;

    return dst;
}
void* memset(void* dst, int val, uint32_t n) {
    val &= 0xFF;

    uint8_t* d = (uint8_t*) dst;
    while (((uintptr_t) d & 0x3) && n--) *d++ = (uint8_t) val;

    uint32_t* d32 = (uint32_t*) d;
    uint32_t val32 = (val << 24) | (val << 16) | (val << 8) | val;
    while (n >= 4) { *d32++ = val32; n -= 4; }

    d = (uint8_t*) d32;
    while (n--) *d++ = (uint8_t) val;

    return dst;
}

void heap_init(uint32_t num_bytes) {
    if (!heap_allocator.buf) {
        arena_init(&heap_allocator, (void*)(__heap_start__), num_bytes);
    }
}
void* malloc(uint32_t num_bytes) {
    return arena_alloc(&heap_allocator, num_bytes);
}
void* malloc_align(uint32_t num_bytes, uint32_t align) {
    return arena_alloc_align(&heap_allocator, num_bytes, align);
}
void free(uint32_t num_bytes) {
    arena_dealloc(&heap_allocator, num_bytes);
}
void free_to(uint32_t pos) {
    arena_dealloc_to(&heap_allocator, pos);
}
uint32_t heap_get_size() {
    return heap_allocator.size;
}

void caches_enable() {
    uint32_t r;
    asm volatile ("MRC p15, 0, %0, c1, c0, 0" : "=r" (r));
    r |= (1 << 12); // l1 instruction cache
    r |= (1 << 11); // branch prediction
    asm volatile ("MCR p15, 0, %0, c1, c0, 0" :: "r" (r));
}

void caches_disable() {
    uint32_t r;
    asm volatile ("MRC p15, 0, %0, c1, c0, 0" : "=r" (r));
    r &= ~(1 << 12); // l1 instruction cache
    r &= ~(1 << 11); // branch prediction
    asm volatile ("MCR p15, 0, %0, c1, c0, 0" :: "r" (r));
}

int errno;
int* __errno() {
    return &errno;
}
