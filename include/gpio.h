#ifndef GPIO_H
#define GPIO_H

#include <stdint.h>
#include <stdbool.h>

#define MAX_PIN_NUM 53

#define GPIO_BASE 0x20200000
#define REG_SIZE_BYTES 4
#define REG_SIZE_BITS 32

#define PINMODE_BITS 3

enum {
    GPFSEL_BASE = GPIO_BASE,
    GPSET_BASE = GPIO_BASE + 0x001C,
    GPCLR_BASE = GPIO_BASE + 0x0028,
    GPLEV_BASE = GPIO_BASE + 0x0034,
    GPEDS_BASE = GPIO_BASE + 0x0040,
    GPREN_BASE = GPIO_BASE + 0x004C,
    GPFEN_BASE = GPIO_BASE + 0x0058,
    GPPUD_BASE = GPIO_BASE + 0x0094,
    GPPUDCLK_BASE = GPIO_BASE + 0x0098,
};

typedef struct {
    uint32_t p_num;
} Pin;

typedef enum {
    GPIO_INPUT = 0,
    GPIO_OUTPUT = 1,
    GPIO_ALT_FUNC_0 = 4,
    GPIO_ALT_FUNC_1 = 5,
    GPIO_ALT_FUNC_2 = 6,
    GPIO_ALT_FUNC_3 = 7,
    GPIO_ALT_FUNC_4 = 3,
    GPIO_ALT_FUNC_5 = 2,
} PinMode;

typedef enum {
    LOW,
    HIGH,
} PinOutput;

typedef enum {
    GPIO_PULL_OFF,
    GPIO_PULL_DOWN,
    GPIO_PULL_UP,
} GpioPull;

void gpio_select(Pin pin, PinMode mode);
void gpio_set(Pin pin, PinOutput value);
PinOutput gpio_read(Pin pin);

void gpio_select_input(Pin pin);
void gpio_select_output(Pin pin);

void gpio_set_low(Pin pin);
void gpio_set_high(Pin pin);

void gpio_set_pull(Pin pin, GpioPull pull);

bool gpio_has_interrupt();
void gpio_enable_int_rising_edge(Pin pin);
void gpio_enable_int_falling_edge(Pin pin);
bool gpio_event_detected(Pin pin);
void gpio_event_clear(Pin pin);

#endif



