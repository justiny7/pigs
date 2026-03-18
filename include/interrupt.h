#ifndef INTERRUPT_H
#define INTERRUPT_H

#define IRQ_BASE 0x2000B000

enum {
    IRQ_BASIC_PENDING = IRQ_BASE + 0x200,
    IRQ_PENDING_1     = IRQ_BASE + 0x204,
    IRQ_PENDING_2     = IRQ_BASE + 0x208,
    IRQ_ENABLE_1      = IRQ_BASE + 0x210,
    IRQ_ENABLE_2      = IRQ_BASE + 0x214,
    IRQ_DISABLE_1     = IRQ_BASE + 0x21C,
    IRQ_DISABLE_2     = IRQ_BASE + 0x220
};

enum {
    AUX_INT    = 29,
    GPIO_INT_0 = 49,
    GPIO_INT_1 = 50,
};

#endif
