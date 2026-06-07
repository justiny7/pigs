#include "lib.h"
#include "math.h"
#include "mmu.h"
#include "sys_timer.h"
#include "gaussian_splat.h"
#include "mailbox_interface.h"
#include "fat.h"
#include "display_font.h"
#include "uart.h"
#include "thread.h"

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
#define INSTRUCTIONS_VIRTUAL_OFFSET (HEIGHT * 2)

#define VERBOSE 1

static const int MiB = 1024 * 1024;
static Arena data_arena;
static GaussianSplat gs;
static FontSettings fs;
static Camera* cam;
static bool rendering, spiral;
static uint32_t instructions;

static uint32_t* pixels;
static uint32_t ply_filesize;
static fatdir_t* ply_fatdir;
static float cur_radius, calc_radius;
static float theta, phi_t;

static volatile Thread menu_thread, render_thread, instructions_thread;
volatile Thread* current_thread;
volatile Thread* next_thread;

// from main.S
void __attribute__((interrupt("IRQ"))) irq_handler();
void __attribute__((interrupt("SWI"))) svc_handler();
void start_thread(volatile uint32_t* sp);
uint32_t get_sp();

void scheduler() {
    // UART interrupt to stop rendering + zoom controls
    if (uart_has_interrupt()) {
        uint32_t aux_irq = GET32(AUX_IRQ);
        if (aux_irq & 1) {
            uint32_t iir = GET32(AUX_MU_IIR_REG);
            if (((iir >> 1) & 3) == 0b10) {
                uint8_t c = GET32(AUX_MU_IO_REG) & 0xFF;
                if (instructions != INSTRUCTIONS_VIRTUAL_OFFSET) {
                    // instructions thread
                    if (c == 'h') {
                        // reset virtual offset
                        mbox_framebuffer_set_virtual_offset(0, instructions);
                        instructions = INSTRUCTIONS_VIRTUAL_OFFSET;

                        if (rendering) {
                            next_thread = &render_thread;
                        } else {
                            next_thread = &menu_thread;
                            uart_disable_interrupts();
                            mem_barrier_dsb();
                        }
                        return;
                    }

                } else if (rendering) {
                    // rendering thread
                    if (c == 'q') {
                        rendering = false;
                        next_thread = &menu_thread;

                        uart_disable_interrupts();
                        mem_barrier_dsb();
                        return;
                    }
                    if (c == 'h') {
                        uint32_t _;
                        mbox_framebuffer_get_virtual_offset(&_, &instructions);
                        mbox_framebuffer_set_virtual_offset(0, INSTRUCTIONS_VIRTUAL_OFFSET);
                        next_thread = &instructions_thread;
                        return;
                    }

                    if (c == ' ') {
                        cur_radius = calc_radius;
                    } else if (c == 'i') {
                        cur_radius = max(cur_radius * 0.9f, calc_radius * 0.66);
                    } else if (c == 'k') {
                        cur_radius = min(cur_radius * 1.1f, calc_radius / 0.66);
                    } else if (c == 'c') {
                        spiral = !spiral;
                        if (phi_t > 0.5 * M_PI) {
                            phi_t = phi_t * 2 - 0.5 * M_PI;
                        }
                    }

                    if (!spiral) {
                        if (c == 'w') {
                            phi_t = max(phi_t - 0.1f, -0.5 * M_PI);
                        } else if (c == 's') {
                            phi_t = min(phi_t + 0.1f, 0.5 * M_PI);
                        } else if (c == 'a') {
                            theta += 0.1;
                        } else if (c == 'd') {
                            theta -= 0.1;
                        }
                    }
                }
            }
        }

        next_thread = current_thread;
    } else {
        // only cooperative switches are menu -> render and menu -> instructions
        if (rendering) {
            next_thread = &render_thread;
        } else {
            uint32_t _;
            mbox_framebuffer_get_virtual_offset(&_, &instructions);
            mbox_framebuffer_set_virtual_offset(0, INSTRUCTIONS_VIRTUAL_OFFSET);

            uart_enable_rx_interrupts();
            mem_barrier_dsb();
            next_thread = &instructions_thread;
        }
    }
}

Vec3 spiral_camera(Vec3 center, float radius) {
    static const float dt = 0.1f;
    static const float omega = 4.0f;

    float phi = M_PI * 0.5f + (M_PI * 0.25f) * sinf(phi_t);
    if (spiral) {
        theta += omega * dt;
        phi_t += dt;
        while (phi_t > M_PI) phi_t -= 2.0f * M_PI;
    }

    Vec3 p;
    p.x = center.x + radius * sinf(phi) * cosf(theta);
    p.y = center.y + radius * cosf(phi);
    p.z = center.z + radius * sinf(phi) * sinf(theta);

    return p;
}

void instructions_loop() {
    // display instructions
    const char* s[] = {
        "INSTRUCTIONS",
        "- In menu, use I/K + Enter to select a model, or Q to quit",
        "- In renderer, use I/K to zoom, Space to reset zoom,",
        "  C + WASD to toggle camera control, and Q to quit to menu",
        "- Use H to toggle this help menu",
    };

    for (uint32_t i = 0; i < 5; i++) {
        uint32_t l = 0;
        for (; s[i][l]; l++);

        display_text(&fs, (const uint8_t*) s[i], l,
                i * (FONT_HEIGHT + 1) * fs.scale + INSTRUCTIONS_VIRTUAL_OFFSET, 0);
    }

    // just an infinite loop bc it's just switching a
    // virtual framebuffer offset (handled in scheduler)
    while (1);
}

void render_loop() {
#if VERBOSE
    printk("Reading PLY...\n");
#endif

    Vec3 center;
    gs_read_ply(&gs, ply_fatdir, ply_filesize, &center, &calc_radius);

    Vec3 cam_up = { { 0.0f, 1.0f, 0.0f } };

#if VERBOSE
    printk("Initializing Camera...\n");
#endif
    // Camera* c = arena_alloc_align(&data_arena, sizeof(Camera), 16);

    // enable UART interrupts (don't wanna interrupt SD card reading)
    // also clear cache in case user was inputting stuff
    uart_flush_tx();
    uart_clear_fifos();
    uart_enable_rx_interrupts();
    mem_barrier_dsb();

    // Orbit around sphere
    cur_radius = calc_radius;
    spiral = true;
    theta = phi_t = 0.0f;
    while (1) {
#if VERBOSE
        uint32_t t = sys_timer_get_usec();
#endif

        Vec3 cam_pos = spiral_camera(center, cur_radius);
        init_camera(cam, cam_pos, center, cam_up, WIDTH, HEIGHT);

        gs_render(&gs, cam);

#if VERBOSE
        uint32_t render_t = sys_timer_get_usec() - t;
        printk("Render time: %d\n", render_t);
#endif
    }
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
    uint32_t num_files;
    uint8_t* lfns;
    fatdir_t* ply_files;
    fat_get_files_ext_lfn("PLY", &ply_files, &lfns, &num_files);

    fs.scale = 2;
    fs.height = HEIGHT * 3;
    fs.width = WIDTH;
    fs.color = 0x00FFFFFF;
    fs.fb = pixels;

    // display instructions in the beginning
    thread_yield();

    while (1) {
        // re-initialize render thread
        thread_init(&render_thread, render_loop);

        for (uint32_t i = 0; i < num_files; i++) {
            uint32_t offset = i * MAX_LFN_LEN;
            if (lfns[offset]) {
                int l = 0;
                for (; lfns[offset + l] && lfns[offset + l] != '.'; l++);
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

            if (input == 'k') {
                flip_row(&fs, selection);
                selection = (selection + 1) % num_files;
                flip_row(&fs, selection);
            } else if (input == 'i') {
                flip_row(&fs, selection);
                selection = (selection + num_files - 1) % num_files;
                flip_row(&fs, selection);
            } else if (input == 'h') {
                thread_yield();
            } else if (input == '\n') {
                break;
            } else if (input == 'q') {
                rpi_reset();
            }
        }

        ply_fatdir = &ply_files[selection];
        ply_filesize = ply_fatdir->size;
        rendering = true;

        uint32_t data_size = data_arena.size;

        thread_yield();

        // wait for QPU kernels to finish
        qpu_block();

        // reset memory
        arena_dealloc_to(&data_arena, data_size);
        gs_reset_arenas(&gs);

        mbox_framebuffer_set_virtual_offset(0, 0);
        memset(pixels, 0, HEIGHT * WIDTH * 2 * sizeof(uint32_t));
        mmu_flush_dcache();
    }
}

void main() {
    // Install ISRs
    extern uint32_t irq_ptr;
    irq_ptr = (uint32_t) irq_handler;

    extern uint32_t swi_ptr;
    swi_ptr = (uint32_t) svc_handler;

    // Enable dcache, icache, BTB
    mmu_enable_caches();

    // Overclock
    mbox_set_clock_rate(MBOX_CLK_ARM, mbox_get_max_clock_rate(MBOX_CLK_ARM));
    mbox_set_clock_rate(MBOX_CLK_V3D, mbox_get_max_clock_rate(MBOX_CLK_V3D));

    // Init heap and data arena
#if VERBOSE
    printk("Initializing heap & data arena...\n");
#endif
    arena_init_qpu(&data_arena, 340 * MiB);
    cam = arena_alloc_align(&data_arena, sizeof(Camera), 16);


    // Init framebuffer
#if VERBOSE
    printk("Initializing framebuffer...\n");
#endif

    uint32_t* fb;
    mbox_framebuffer_init(WIDTH, HEIGHT, WIDTH, HEIGHT * 3, 32, &fb); // double buffer
    pixels = (uint32_t*) TO_CPU(fb);

#if VERBOSE
    printk("Initializing SD card / FAT32...\n");
#endif
    // Init SD card / FAT32
    fat_init();
    
#if VERBOSE
    printk("Initializing font...\n");
#endif
    // Initialize font
    font_init();

    // Initialize Gaussian splat
#if VERBOSE
    printk("Initializing Gaussian splat...\n");
#endif
    gs_init(&gs, &data_arena, pixels, NUM_QPUS);

    // Init threads
    thread_init(&menu_thread, menu_loop);
    thread_init(&instructions_thread, instructions_loop);

    current_thread = &menu_thread;
    start_thread(current_thread->sp);

    // should never get here
    rpi_reset();
}
