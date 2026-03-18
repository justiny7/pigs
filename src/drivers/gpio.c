#include "lib.h"
#include "gpio.h"
#include "sys_timer.h"
#include "interrupt.h"

uint32_t _check_pin(Pin pin) {
    const uint32_t p_num = pin.p_num;
    assert(p_num <= MAX_PIN_NUM, "Illegal GPIO pin");

    return p_num;
}

void gpio_select(Pin pin, PinMode mode) {
    const uint32_t p_num = _check_pin(pin);
    assert((mode & 0b111) == mode, "Invalid GPIO mode");
    
    const uint32_t register_offset = REG_SIZE_BYTES * (p_num / 10);
    const uint32_t register_base = GPFSEL_BASE + register_offset;

    const uint32_t bit_offset = PINMODE_BITS * (p_num % 10);

    uint32_t value = GET32(register_base);
    value &= ~(0x7U << bit_offset);
    value |= (mode << bit_offset);
    PUT32(register_base, value);
}

void gpio_set(Pin pin, PinOutput value) {
    const uint32_t p_num = _check_pin(pin);

    const uint32_t register_base = (value == LOW ? GPCLR_BASE : GPSET_BASE);
    const uint32_t register_offset = (p_num >> 5) * REG_SIZE_BYTES;
    const uint32_t val_bit = 1U << (p_num & 31);

    PUT32(register_base + register_offset, val_bit);
}

PinOutput gpio_read(Pin pin) {
    const uint32_t p_num = _check_pin(pin);

    const uint32_t offset = (p_num >> 5) * REG_SIZE_BYTES;
    const uint32_t shift = p_num & 31;

    return (PinOutput) ((GET32(GPLEV_BASE + offset) >> shift) & 1);
}

void gpio_select_input(Pin pin) {
    gpio_select(pin, GPIO_INPUT);
}
void gpio_select_output(Pin pin) {
    gpio_select(pin, GPIO_OUTPUT);
}

void gpio_set_low(Pin pin) {
    gpio_set(pin, LOW);
}
void gpio_set_high(Pin pin) {
    gpio_set(pin, HIGH);
}

void gpio_set_pull(Pin pin, GpioPull pull) {
    const uint32_t p_num = _check_pin(pin);

    const uint32_t offset = REG_SIZE_BYTES * (p_num >> 5);
    const uint32_t shift = p_num & 31;

    PUT32(GPPUD_BASE, pull);
    sys_timer_delay_us(150);
    PUT32(GPPUDCLK_BASE + offset, 1U << shift);
    sys_timer_delay_us(150);
    PUT32(GPPUD_BASE, 0);
    PUT32(GPPUDCLK_BASE + offset, 0);
}

bool gpio_has_interrupt() {
    mem_barrier_dsb();
    bool res = GET32(IRQ_PENDING_2) & (1U << (GPIO_INT_0 - 32));
    mem_barrier_dsb();
    return res;
}

void gpio_enable_int(Pin pin) {
    const uint32_t p_num = _check_pin(pin);

    mem_barrier_dsb();
    PUT32(IRQ_ENABLE_2, (1U << (GPIO_INT_0 + (p_num >= 32) - 32)));
    mem_barrier_dsb();
}
void gpio_enable_int_rising_edge(Pin pin) {
    const uint32_t p_num = _check_pin(pin);

    const uint32_t register_offset = (p_num >> 5) * REG_SIZE_BYTES;
    const uint32_t val_bit = 1U << (p_num & 31);

    mem_barrier_dsb();

    OR32(GPREN_BASE + register_offset, val_bit);

    gpio_enable_int(pin);
}
void gpio_enable_int_falling_edge(Pin pin) {
    const uint32_t p_num = _check_pin(pin);

    const uint32_t register_offset = (p_num >> 5) * REG_SIZE_BYTES;
    const uint32_t val_bit = 1U << (p_num & 31);

    mem_barrier_dsb();

    OR32(GPFEN_BASE + register_offset, val_bit);

    gpio_enable_int(pin);
}
bool gpio_event_detected(Pin pin) {
    const uint32_t p_num = _check_pin(pin);

    const uint32_t offset = (p_num >> 5) * REG_SIZE_BYTES;
    const uint32_t shift = p_num & 31;

    mem_barrier_dsb();
    bool res = (GET32(GPEDS_BASE + offset) >> shift) & 1;
    mem_barrier_dsb();

    return res;
}
void gpio_event_clear(Pin pin) {
    const uint32_t p_num = _check_pin(pin);

    const uint32_t register_offset = (p_num >> 5) * REG_SIZE_BYTES;
    const uint32_t val_bit = 1U << (p_num & 31);

    mem_barrier_dsb();
    PUT32(GPEDS_BASE + register_offset, val_bit);
    mem_barrier_dsb();
}
