#include "uart.h"
#include "lib.h"

void __attribute__((interrupt("IRQ"))) uart_irq_handler() {
    if (uart_has_interrupt()) {
        uint32_t aux_irq = GET32(AUX_IRQ);
        if (aux_irq & 1) {
            uint32_t iir = GET32(AUX_MU_IIR_REG);
            if (((iir >> 1) & 3) == 0b10) {
                uint8_t c = GET32(AUX_MU_IO_REG) & 0xFF;
                PUT32(AUX_MU_IO_REG, (uint32_t) c);

                if (c == 'q') {
                    uart_disable_interrupts();
                    mem_barrier_dsb();
                }
            }
        }
    }
}

void main() {
    extern uint32_t interrupt_handler_ptr;
    interrupt_handler_ptr = (uint32_t) uart_irq_handler;

    uart_enable_rx_interrupts();
    mem_barrier_dsb();

    while (1);

    rpi_reset();
}
