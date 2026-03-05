#include "heap_allocator.h"
#include "uart.h"
#include "lib.h"
#include "debug.h"

extern uint32_t __heap_start__;

void main() {
    heap_init();
    uart_puts("Heap initialized\n");
    DEBUG_X(__heap_start__);
    DEBUG_X(heap_buf);
    DEBUG_D(heap_size);
    DEBUG_D(heap_capacity);

    uint32_t p1 = (uint32_t) heap_alloc(1024);
    uart_puts("Heap allocated 1024 bytes\n");
    DEBUG_X(p1);
    DEBUG_D(heap_size);
    DEBUG_D(heap_capacity);

    uint32_t p2 = (uint32_t) heap_alloc(1024);
    uart_puts("Heap allocated 1024 bytes\n");
    DEBUG_X(p2);
    DEBUG_D(heap_size);
    DEBUG_D(heap_capacity);

    heap_reset();
    uart_puts("Heap reset\n");

    rpi_reset();
}