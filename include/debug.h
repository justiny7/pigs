#ifndef DEBUG_H
#define DEBUG_H

#include "uart.h"

#ifdef DEBUG
#define DEBUG_D(var) \
    do { \
        uart_puts("[DEBUG] " #var ": "); \
        uart_putd((uint32_t) (var)); \
        uart_puts("\n"); \
    } while (0)
#define DEBUG_X(var) \
    do { \
        uart_puts("[DEBUG] " #var ": "); \
        uart_putx((uint32_t) (var)); \
        uart_puts("\n"); \
    } while (0)
#define DEBUG_F(var) \
    do { \
        uart_puts("[DEBUG] " #var ": "); \
        uart_putf((float) (var)); \
        uart_puts("\n"); \
    } while (0)


#define DEBUG_DM(var, msg) \
    do { \
        uart_puts("[DEBUG] "); \
        uart_puts(msg); \
        uart_puts(": "); \
        uart_putd((uint32_t) (var)); \
        uart_puts("\n"); \
    } while (0)
#define DEBUG_XM(var, msg) \
    do { \
        uart_puts("[DEBUG] "); \
        uart_puts(msg); \
        uart_puts(": "); \
        uart_putx((uint32_t) (var)); \
        uart_puts("\n"); \
    } while (0)
#define DEBUG_FM(var, msg) \
    do { \
        uart_puts("[DEBUG] "); \
        uart_puts(msg); \
        uart_puts(": "); \
        uart_putf((float) (var)); \
        uart_puts("\n"); \
    } while (0)
#else
#define DEBUG_D(var)
#define DEBUG_X(var)
#define DEBUG_F(var)
#define DEBUG_DM(var)
#define DEBUG_XM(var)
#define DEBUG_FM(var)
#endif

#endif
