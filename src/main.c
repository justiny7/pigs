#include "lib.h"
#include "math.h"
#include "mmu.h"
#include "sys_timer.h"
#include "gaussian_splat.h"
#include "mailbox_interface.h"
#include "fat.h"
#include "display_font.h"
#include "uart.h"

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

// #define VERBOSE

const int MiB = 1024 * 1024;
Arena data_arena;
GaussianSplat gs;
bool rendering;
uint32_t* pixels;
uint32_t ply_cluster, filesize;
float cur_radius, calc_radius;

// UART interrupt to stop rendering + zoom controls
void __attribute__((interrupt("IRQ"))) uart_irq_handler() {
    if (uart_has_interrupt()) {
        uint32_t aux_irq = GET32(AUX_IRQ);
        if (aux_irq & 1) {
            uint32_t iir = GET32(AUX_MU_IIR_REG);
            if (((iir >> 1) & 3) == 0b10) {
                uint8_t c = GET32(AUX_MU_IO_REG) & 0xFF;
                if (c == 'q') {
                    rendering = false;
                    uart_disable_interrupts();
                    mem_barrier_dsb();
                } else if (c == ' ') {
                    cur_radius = calc_radius;
                } else if (c == 'w') {
                    cur_radius = max(cur_radius * 0.9f, calc_radius * 0.66);
                } else if (c == 's') {
                    cur_radius = min(cur_radius * 1.1f, calc_radius / 0.66);
                }
            }
        }
    }
}

Vec3 spiral_camera(Vec3 center, float radius, bool reset) {
    static float theta = 0.f, t = 0.f;
    static const float dt = 0.1f;
    static const float omega = 4.0f;

    if (reset) {
        theta = t = 0.0f;
    }

    float phi = M_PI * 0.5f + (M_PI * 0.25f) * sinf(t);
    theta += omega * dt;

    Vec3 p;
    p.x = center.x + radius * sinf(phi) * cosf(theta);
    p.y = center.y + radius * cosf(phi);
    p.z = center.z + radius * sinf(phi) * sinf(theta);

    t += dt;

    return p;
}

void render_loop() {
    uint32_t heap_size = heap_get_size();
    uint32_t data_size = data_arena.size;

#ifdef VERBOSE
    uart_puts("Reading PLY...\n");
#endif

    Vec3 center;
    gs_read_ply(&gs, ply_cluster, filesize, &center, &calc_radius);

    Vec3 cam_up = { { 0.0f, 1.0f, 0.0f } };

#ifdef VERBOSE
    uart_puts("Initializing Camera...\n");
#endif
    Camera* c = arena_alloc_align(&data_arena, sizeof(Camera), 16);

    // enable UART interrupts
    uart_enable_rx_interrupts();
    mem_barrier_dsb();

    // Orbit around sphere
    bool reset = true;
    cur_radius = calc_radius;
    while (rendering) {
#ifdef VERBOSE
        uint32_t t = sys_timer_get_usec();
#endif

        Vec3 cam_pos = spiral_camera(center, cur_radius, reset);
        init_camera(c, cam_pos, center, cam_up, WIDTH, HEIGHT);

        gs_set_camera(&gs, c);
        gs_render(&gs);

#ifdef VERBOSE
        uint32_t render_t = sys_timer_get_usec() - t;
        uart_puts("Render time: ");
        uart_putd(render_t);
        uart_puts("\n");
#endif

        reset = false;
    }

    free_to(heap_size);
    arena_dealloc_to(&data_arena, data_size);
    gs_reset_arenas(&gs);

    mbox_framebuffer_set_virtual_offset(0, 0);
    memset(pixels, 0, HEIGHT * WIDTH * 2 * sizeof(uint32_t));
    mmu_flush_dcache();
}

void flip_row(FontSettings* fs, uint32_t row) {
    for (uint32_t i = 0; i < FONT_HEIGHT * fs->scale; i++) {
        for (uint32_t j = 0; j < WIDTH; j++) {
            uint32_t idx = (row * (FONT_HEIGHT + 1) * fs->scale + i) * WIDTH + j;
            pixels[idx] ^= 0x00FFFFFF;
        }
    }

    mmu_flush_dcache();
}
void menu_loop() {
    uart_puts("INSTRUCTIONS:\n");
    uart_puts("- Use W/S + Enter to select a model, or Q to quit\n");
    uart_puts("- Use W/S to zoom in/out, Space to reset zoom, or Q to quit\n");
    uint32_t num_files;
    uint8_t* lfns;
    fatdir_t* ply_files;
    fat_get_plys(&ply_files, &lfns, &num_files);

    FontSettings fs;
    fs.scale = 4;
    fs.height = HEIGHT;
    fs.width = WIDTH;
    fs.color = 0x00FFFFFF;
    fs.fb = pixels;

    while (1) {
        for (uint32_t i = 0; i < num_files; i++) {
            uint32_t offset = i * MAX_LFN_LEN;
            if (lfns[offset]) {
                int l = 0;
                for (; lfns[offset + l] && lfns[offset + l] != '.'; l++) {
                    if (lfns[offset + l] >= 'A' && lfns[offset + l] <= 'Z') {
                        lfns[offset + l] += 'a' - 'A';
                    }
                }
                display_text(&fs, &lfns[offset], l, i * (FONT_HEIGHT + 1) * fs.scale, 0);
            } else {
                for (int j = 0; j < 8; j++) {
                    if (ply_files[i].name[j] >= 'A' && ply_files[i].name[j] <= 'Z') {
                        ply_files[i].name[j] += 'a' - 'A';
                    }
                }
                display_text(&fs, ply_files[i].name, 8, i * (FONT_HEIGHT + 1) * fs.scale, 0);
            }
        }

        uint32_t selection = 0;
        flip_row(&fs, selection);

        while (1) {
            uint8_t input = uart_getc();

            if (input == 's') {
                flip_row(&fs, selection);
                selection = (selection + 1) % num_files;
                flip_row(&fs, selection);
            } else if (input == 'w') {
                flip_row(&fs, selection);
                selection = (selection + num_files - 1) % num_files;
                flip_row(&fs, selection);
            } else if (input == '\n') {
                break;
            } else if (input == 'q') {
                rpi_reset();
            }
        }

        ply_cluster = fatdir_get_cluster(&ply_files[selection]);
        filesize = ply_files[selection].size;
        rendering = true;

        render_loop();
    }
}

void main() {
    // Install UART interrupts
    extern uint32_t interrupt_handler_ptr;
    interrupt_handler_ptr = (uint32_t) uart_irq_handler;

    page_table_init();
    mmu_init();
    mmu_enable();

    // Enable dcache, icache, BTB
    mmu_enable_caches();

    // Overclock
    mbox_set_clock_rate(MBOX_CLK_ARM, mbox_get_max_clock_rate(MBOX_CLK_ARM));
    mbox_set_clock_rate(MBOX_CLK_V3D, mbox_get_max_clock_rate(MBOX_CLK_V3D));

    // Init heap and data arena
#ifdef VERBOSE
    uart_puts("Initializing heap & data arena...\n");
#endif
    heap_init(70 * MiB);
    arena_init_qpu(&data_arena, 350 * MiB);

    // Init framebuffer
#ifdef VERBOSE
    uart_puts("Initializing framebuffer...\n");
#endif

    uint32_t* fb;
    mbox_framebuffer_init(WIDTH, HEIGHT, WIDTH, HEIGHT * 2, 32, &fb); // double buffer
    pixels = (uint32_t*) TO_CPU(fb);

#ifdef VERBOSE
    uart_puts("Initializing SD card / FAT32...\n");
#endif
    // Init SD card / FAT32
    fat_init();
    
#ifdef VERBOSE
    uart_puts("Initializing font...\n");
#endif
    // Initialize font
    font_init();

    // Initialize Gaussian splat
#ifdef VERBOSE
    uart_puts("Initializing Gaussian splat...\n");
#endif
    gs_init(&gs, &data_arena, pixels, NUM_QPUS);

    menu_loop();

    rpi_reset();
}
