#ifndef LIB_H
#define LIB_H

#include <stdint.h>
#include <stdbool.h>

void PUT32(uint32_t addr, uint32_t val);
uint32_t GET32(uint32_t addr);
void OR32(uint32_t addr, uint32_t val);

void PUT8(uint32_t addr, uint32_t val); // stores lowest 8 bytes of val
uint32_t GET8(uint32_t addr);

void mem_barrier_dsb();
void mem_barrier_dmb();

void rpi_reboot();
void rpi_reset();

void assert(bool val, const char* msg);
void panic(const char* msg);

void* memcpy(void* dst, const void* src, uint32_t n);
void* memset(void* dst, int val, uint32_t n);

void caches_enable();
void caches_disable();

#endif
