#include "math.h"

float sqrtf(float x) {
    float result;
    // 't' constraint tells GCC to use a VFP register (s0-s31)
    __asm__ volatile ("vsqrt.f32 %0, %1" : "=t"(result) : "t"(x));
    return result;
}

float expf(float x) {
    // Handle extreme values
    if (x < -20.0f) return 0.0f;
    if (x > 20.0f) return 1e9f;
    
    // Range reduction: exp(x) = exp(x/n)^n
    // Reduce x to [-1, 1] range for better Taylor convergence
    int n = 1;
    float r = x;
    while (r > 1.0f || r < -1.0f) {
        r *= 0.5f;
        n *= 2;
    }
    
    // Taylor series for exp(r) where |r| <= 1
    // 1 + r + r^2/2! + r^3/3! + r^4/4! + r^5/5! + r^6/6! + r^7/7!
    float res = 1.0f + r * (1.0f + r * (0.5f + r * (0.16666667f + 
                r * (0.04166667f + r * (0.00833333f + r * (0.00138889f + r * 0.0001984127f))))));
    
    // Square n times to recover exp(x)
    while (n > 1) {
        res = res * res;
        n /= 2;
    }
    
    return res;
}

float sinf(float x) {
    // Wrap x to [-PI, PI]
    while (x >  M_PI) x -= 2.0f * M_PI;
    while (x < -M_PI) x += 2.0f * M_PI;

    // 7th order Taylor: x - x^3/3! + x^5/5! - x^7/7!
    float x2 = x * x;
    return x * (1.0f - x2 * (0.16666667f - x2 * (0.00833333f - x2 * 0.0001984126f)));
}

float cosf(float x) {
    return sinf(x + (M_PI / 2.0f));
}

float tanf(float x) {
    return sinf(x) / cosf(x);
}
