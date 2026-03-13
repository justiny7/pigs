#include "lib.h"
#include "debug.h"
#include "math.h"
#include "mmu.h"
#include "sys_timer.h"
#include "gaussian_splat.h"
#include "mailbox_interface.h"

// #define WIDTH 1024
// #define HEIGHT 1024
#define WIDTH 1280
#define HEIGHT 720
// #define WIDTH 640
// #define HEIGHT 480
// #define WIDTH 256
// #define HEIGHT 192
// #define WIDTH 128
// #define HEIGHT 96

#define NUM_QPUS 12

#define FILE_NAME "FLY     PLY"
// #define FILE_NAME "VASE    PLY"
// #define FILE_NAME "CACTUS  PLY"
// #define FILE_NAME "BONSAI  PLY"
// #define FILE_NAME "GOLDORAKPLY"
// #define FILE_NAME "MOTOR~44PLY"

#define VERBOSE

Arena data_arena;
GaussianSplat gs;

Vec3 spiral_camera(Vec3 center, float radius) {
    static float theta = 0.0f;
    static float t = 0.0f;
    static const float dt = 0.1f;
    static const float omega = 4.0f;

    float phi = M_PI * 0.5f + (M_PI * 0.25f) * sinf(t);
    theta += omega * dt;

    Vec3 p;
    p.x = center.x + radius * sinf(phi) * cosf(theta);
    p.y = center.y + radius * cosf(phi);
    p.z = center.z + radius * sinf(phi) * sinf(theta);

    t += dt;

    return p;
}

void main() {
    page_table_init();
    mmu_init();
    mmu_enable();

    // Enable dcache, icache, BTB
    mmu_enable_caches();

    // Overclock
    mbox_set_clock_rate(MBOX_CLK_ARM, mbox_get_max_clock_rate(MBOX_CLK_ARM));
    mbox_set_clock_rate(MBOX_CLK_V3D, mbox_get_max_clock_rate(MBOX_CLK_V3D));

    uart_puts("Initializing framebuffer...\n");

    uint32_t* fb;
    mbox_framebuffer_init(WIDTH, HEIGHT, WIDTH, HEIGHT * 2, 32, &fb); // double buffer
    mbox_framebuffer_set_virtual_offset(0, HEIGHT);

    uint32_t* pixels = (uint32_t*)((uintptr_t)fb & 0x3FFFFFFF);

    uart_puts("Initializing data arena...\n");
    const int MiB = 1024 * 1024;
    arena_init_qpu(&data_arena, 350 * MiB);

    uart_puts("Initializing Gaussian splat...\n");
    gs_init(&gs, &data_arena, pixels, NUM_QPUS);

    float x_avg, y_avg, z_avg;
    gs_read_ply(&gs, FILE_NAME, &x_avg, &y_avg, &z_avg);

    Vec3 cam_target = { { x_avg, y_avg, z_avg} };
    Vec3 cam_up = { { 0.0f, 1.0f, 0.0f } };

    uart_puts("Initializing Camera...\n");
    Camera* c = arena_alloc_align(&data_arena, sizeof(Camera), 16);

    // Orbit around sphere
    // float rad = 5.0;
    float rad = 0.75;
    Vec3 center = { { x_avg, y_avg, z_avg } };
    while (1) {
#ifdef VERBOSE
        uint32_t t = sys_timer_get_usec();
#endif

        Vec3 cam_pos = spiral_camera(center, rad);
        init_camera(c, cam_pos, cam_target, cam_up, WIDTH, HEIGHT);

        gs_set_camera(&gs, c);
        gs_render(&gs);

#ifdef VERBOSE
        uint32_t render_t = sys_timer_get_usec() - t;
        uart_puts("Render time: ");
        uart_putd(render_t);
        uart_puts("\n");
#endif
    }

    rpi_reset();
}
