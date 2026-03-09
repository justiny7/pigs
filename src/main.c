#include "lib.h"
#include "debug.h"
#include "math.h"
#include "sys_timer.h"
#include "gaussian_splat.h"
#include "mailbox_interface.h"

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
// #define FILE_NAME "CACTUS  PLY"
// #define FILE_NAME "BONSAI  PLY"

Arena data_arena;
GaussianSplat gs;

void main() {
    uint32_t *fb;
    uint32_t size, pitch;

    uart_puts("Initializing framebuffer...\n");
    mbox_framebuffer_init(WIDTH, HEIGHT, 32, &fb, &size, &pitch);
    if (pitch != WIDTH * 4) {
        panic("Framebuffer init failed!");
    }

    uint32_t* pixels = (uint32_t*)((uintptr_t)fb & 0x3FFFFFFF);

    const int MiB = 1024 * 1024;
    arena_init(&data_arena, MiB * 225);

    gs_init(&gs, &data_arena, pixels, NUM_QPUS);

    float x_avg, y_avg, z_avg;
    gs_read_ply(&gs, FILE_NAME, &x_avg, &y_avg, &z_avg);

    Vec3 cam_target = { { x_avg, y_avg, z_avg} };
    Vec3 cam_up = { { 0.0f, 1.0f, 0.0f } };

    Camera* c = arena_alloc_align(&data_arena, sizeof(Camera), 16);

    // Interpolate circle around center
    float rad = 0.75;
    // float rad = 5.0;
    float height = 1.0;
    float angle = 0.0;
    while (1) {
        uart_puts("Angle: ");
        uart_putf(angle * 180 / M_PI);
        uart_puts("\n");

        Vec3 cam_pos = (Vec3) { {
            x_avg + rad * cosf(angle),
            y_avg - height,
            z_avg + rad * sinf(angle) } };
        init_camera(c, cam_pos, cam_target, cam_up, WIDTH, HEIGHT);

        gs_set_camera(&gs, c);
        gs_render(&gs);

        angle += 2.0f * M_PI / 30;
        if (angle > M_PI) {
            angle -= 2.0 * M_PI;
        }
    }

    caches_disable();
    rpi_reset();
}
