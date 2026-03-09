#include "gpio.h"
#include "uart.h"
#include "lib.h"
#include "sys_timer.h"
#include "mailbox_interface.h"
#include "gaussian.h"
#include "math.h"
#include "debug.h"
#include "kernel.h"

#include "project_points_cov2d_inv.h"
#include "spherical_harmonics.h"
#include "calc_bbox.h"
#include "calc_tile.h"
#include "sort_copy.h"
#include "render.h"
#include "scan_rot.h"
#include "scan_sum.h"

// #define WIDTH 1280
// #define HEIGHT 720
#define WIDTH 640
#define HEIGHT 480
// #define WIDTH 256
// #define HEIGHT 192
// #define WIDTH 128
// #define HEIGHT 96
#define TILE_SIZE 16
#define NUM_TILES (WIDTH * HEIGHT / (TILE_SIZE * TILE_SIZE))

#define NUM_QPUS 1
#define SIMD_WIDTH 16

Arena data_arena;
Kernel project_points_k, sh_k, bbox_k, tile_k, sort_copy_k, render_k;
Kernel scan_rot_k, scan_sum_k;

static uint32_t rand_state = 12345;

static uint32_t rand_u32(void) {
    rand_state = rand_state * 1103515245 + 12345;
    return rand_state;
}

static float rand_float(float min, float max) {
    return min + (float)(rand_u32() & 0xFFFF) / 65535.0f * (max - min);
}

void qpu_scan(uint32_t* arr, int n) {
    assert((n & 0xF) == 0, "N must be divisble by 16 for scan");

    kernel_reset_unifs(&scan_rot_k);

    for (int q = 0; q < NUM_QPUS; q++) {
        kernel_load_unif(&scan_rot_k, q, NUM_QPUS * SIMD_WIDTH);
        kernel_load_unif(&scan_rot_k, q, n);
        kernel_load_unif(&scan_rot_k, q, q);

        kernel_load_unif(&scan_rot_k, q, TO_BUS(arr + q * SIMD_WIDTH));
    }

    kernel_execute(&scan_rot_k);

    uint32_t* pref = arena_alloc_align(&data_arena, n * sizeof(uint32_t), 16);
    pref[0] = 0;
    for (int i = 0; i + SIMD_WIDTH < n; i += SIMD_WIDTH) {
        pref[i + SIMD_WIDTH] = pref[i] + arr[i + SIMD_WIDTH - 1];
    }

    kernel_reset_unifs(&scan_sum_k);

    for (int q = 0; q < NUM_QPUS; q++) {
        kernel_load_unif(&scan_sum_k, q, NUM_QPUS * SIMD_WIDTH);
        kernel_load_unif(&scan_sum_k, q, n);
        kernel_load_unif(&scan_sum_k, q, q);

        kernel_load_unif(&scan_sum_k, q, TO_BUS(arr + q * SIMD_WIDTH));
        kernel_load_unif(&scan_sum_k, q, TO_BUS(pref + q * SIMD_WIDTH));
    }

    kernel_execute(&scan_sum_k);
}

void sort(ProjectedGaussianPtr* pg, int n, ProjectedGaussianPtr* orig, uint32_t* gaussians_touched) {
    uint32_t t;

    // key = tile (12 bits) | ~(upper 20 bits of depth)
    // depth bits are flipped bc we want depth descending
    // since we only have 12 bits for tile, we can only have max 4096 tiles
    uint32_t* temp_key = arena_alloc_align(&data_arena, n * sizeof(uint32_t), 16);
    uint32_t* temp_id = arena_alloc_align(&data_arena, n * sizeof(uint32_t), 16);

    uint32_t* cnt = arena_alloc_align(&data_arena, (1 << 16) * sizeof(uint32_t), 16);
    uint32_t* cnt2 = arena_alloc_align(&data_arena, (1 << 16) * sizeof(uint32_t), 16);
    memset(cnt, 0, sizeof(cnt));
    memset(cnt2, 0, sizeof(cnt2));

    t = sys_timer_get_usec();
    {   // pass 1
        for (int i = 0; i < n; i++) {
            uint32_t key = pg->depth_key[i].key;

            cnt[key & 0xFFFF]++;
            gaussians_touched[(key >> 20) + 1]++;
        }

        qpu_scan(cnt, 1 << 16);

        for (int i = n - 1; i >= 0; i--) {
            uint32_t key = pg->depth_key[i].key;
            int j = key & 0xFFFF;
            temp_key[cnt[j] - 1] = (key >>= 16);
            temp_id[--cnt[j]] = pg->radius_id[i].id;
            cnt2[key]++;
        }
    }
    uint32_t iter1_tot = sys_timer_get_usec() - t;
    DEBUG_D(iter1_tot);

    t = sys_timer_get_usec();
    {   // pass 2: no need to copy key
        qpu_scan(cnt2, 1 << 16);

        for (int i = n - 1; i >= 0; i--) {
            pg->radius_id[--cnt2[temp_key[i]]].id = temp_id[i];
        }

        kernel_reset_unifs(&sort_copy_k);

        for (int q = 0; q < NUM_QPUS; q++) {
            kernel_load_unif(&sort_copy_k, q, NUM_QPUS * SIMD_WIDTH);
            kernel_load_unif(&sort_copy_k, q, n);
            kernel_load_unif(&sort_copy_k, q, q);

            kernel_load_unif(&sort_copy_k, q, TO_BUS(pg->radius_id + q * SIMD_WIDTH));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(pg->screen_x + q * SIMD_WIDTH));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(pg->screen_y + q * SIMD_WIDTH));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(pg->cov2d_inv_x + q * SIMD_WIDTH));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(pg->cov2d_inv_y + q * SIMD_WIDTH));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(pg->cov2d_inv_z + q * SIMD_WIDTH));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(pg->color_r + q * SIMD_WIDTH));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(pg->color_g + q * SIMD_WIDTH));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(pg->color_b + q * SIMD_WIDTH));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(pg->opacity + q * SIMD_WIDTH));

            kernel_load_unif(&sort_copy_k, q, TO_BUS(orig->screen_x));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(orig->screen_y));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(orig->cov2d_inv_x));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(orig->cov2d_inv_y));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(orig->cov2d_inv_z));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(orig->color_r));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(orig->color_g));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(orig->color_b));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(orig->opacity));
        }

        kernel_execute(&sort_copy_k);
    }
    uint32_t iter2_tot = sys_timer_get_usec() - t;
    DEBUG_D(iter2_tot);

    t = sys_timer_get_usec();
    for (int i = 0; i < NUM_TILES; i++) {
        gaussians_touched[i + 1] += gaussians_touched[i];
    }
    uint32_t prefsum_tot = sys_timer_get_usec() - t;
    DEBUG_D(prefsum_tot);
}

void precompute_gaussians_qpu(Camera* c, GaussianPtr* g, ProjectedGaussianPtr* pg, uint32_t num_gaussians) {
    //////// PROJECT POINTS + COV2D INV
    kernel_reset_unifs(&project_points_k);

    for (uint32_t q = 0; q < NUM_QPUS; q++) {
        kernel_load_unif(&project_points_k, q, NUM_QPUS * SIMD_WIDTH);
        kernel_load_unif(&project_points_k, q, num_gaussians);
        kernel_load_unif(&project_points_k, q, q);
        kernel_load_unif(&project_points_k, q, TO_BUS(g->pos_x + q * SIMD_WIDTH));
        kernel_load_unif(&project_points_k, q, TO_BUS(g->pos_y + q * SIMD_WIDTH));
        kernel_load_unif(&project_points_k, q, TO_BUS(g->pos_z + q * SIMD_WIDTH));
        kernel_load_unif(&project_points_k, q, TO_BUS(pg->depth_key + q * SIMD_WIDTH));
        kernel_load_unif(&project_points_k, q, TO_BUS(pg->screen_x + q * SIMD_WIDTH));
        kernel_load_unif(&project_points_k, q, TO_BUS(pg->screen_y + q * SIMD_WIDTH));
        kernel_load_unif(&project_points_k, q, TO_BUS(pg->radius_id + q * SIMD_WIDTH));

        //  in order that we need for computation
        for (uint32_t i = 0; i < 12; i++) {
            kernel_load_unif(&project_points_k, q, c->w2c.m[i]);
        }
        kernel_load_unif(&project_points_k, q, c->fx);
        kernel_load_unif(&project_points_k, q, c->cx);
        kernel_load_unif(&project_points_k, q, c->fy);
        kernel_load_unif(&project_points_k, q, c->cy);

        for (int i = 0; i < 6; i++) {
            kernel_load_unif(&project_points_k, q, TO_BUS(g->cov3d[i] + q * SIMD_WIDTH));
        }

        kernel_load_unif(&project_points_k, q, TO_BUS(pg->cov2d_inv_x + q * SIMD_WIDTH));
        kernel_load_unif(&project_points_k, q, TO_BUS(pg->cov2d_inv_y + q * SIMD_WIDTH));
        kernel_load_unif(&project_points_k, q, TO_BUS(pg->cov2d_inv_z + q * SIMD_WIDTH));
    }

    uint32_t t;

    t = sys_timer_get_usec();
    kernel_execute(&project_points_k);
    uint32_t project_points_kernel_t  = sys_timer_get_usec() - t;
    DEBUG_D(project_points_kernel_t);

    kernel_free(&project_points_k);

    ////////// SPHERICAL HARMONICS
    kernel_reset_unifs(&sh_k);
    float** sh[3] = {
        g->sh_x,
        g->sh_y,
        g->sh_z
    };
    float* colors[3] = {
        pg->color_r,
        pg->color_g,
        pg->color_b
    };

    for (uint32_t q = 0; q < NUM_QPUS; q++) {
        kernel_load_unif(&sh_k, q, NUM_QPUS * SIMD_WIDTH);
        kernel_load_unif(&sh_k, q, num_gaussians);
        kernel_load_unif(&sh_k, q, q);

        kernel_load_unif(&sh_k, q, TO_BUS(g->pos_x + q * SIMD_WIDTH));
        kernel_load_unif(&sh_k, q, TO_BUS(g->pos_y + q * SIMD_WIDTH));
        kernel_load_unif(&sh_k, q, TO_BUS(g->pos_z + q * SIMD_WIDTH));

        kernel_load_unif(&sh_k, q, c->pos.x);
        kernel_load_unif(&sh_k, q, c->pos.y);
        kernel_load_unif(&sh_k, q, c->pos.z);

        for (uint32_t c = 0; c < 3; c++) {
            kernel_load_unif(&sh_k, q, TO_BUS(colors[c] + q * SIMD_WIDTH));
            for (uint32_t i = 0; i < 16; i++) {
                kernel_load_unif(&sh_k, q, TO_BUS(sh[c][i] + q * SIMD_WIDTH));
            }
        }
    }

    t = sys_timer_get_usec();
    kernel_execute(&sh_k);
    uint32_t sh_kernel_t = sys_timer_get_usec() - t;
    DEBUG_D(sh_kernel_t);

    kernel_free(&sh_k);
}

void count_intersections(ProjectedGaussianPtr* pg, int n,
        uint32_t* tiles_touched,
        ProjectedGaussianPtr* pg_all) {
    uint32_t t;

    /////////// CALCULATE BBOX
    kernel_reset_unifs(&bbox_k);

    for (uint32_t q = 0; q < NUM_QPUS; q++) {
        kernel_load_unif(&bbox_k, q, NUM_QPUS * SIMD_WIDTH);
        kernel_load_unif(&bbox_k, q, n);
        kernel_load_unif(&bbox_k, q, q);

        kernel_load_unif(&bbox_k, q, WIDTH / TILE_SIZE - 1);
        kernel_load_unif(&bbox_k, q, HEIGHT / TILE_SIZE - 1);

        kernel_load_unif(&bbox_k, q, TO_BUS(pg->screen_x + q * SIMD_WIDTH));
        kernel_load_unif(&bbox_k, q, TO_BUS(pg->screen_y + q * SIMD_WIDTH));
        kernel_load_unif(&bbox_k, q, TO_BUS(pg->radius_id + q * SIMD_WIDTH));

        kernel_load_unif(&bbox_k, q, TO_BUS(tiles_touched + 1 + q * SIMD_WIDTH));
    }

    t = sys_timer_get_usec();
    kernel_execute(&bbox_k);
    uint32_t bbox_kernel_t = sys_timer_get_usec() - t;
    DEBUG_D(bbox_kernel_t);

    kernel_free(&bbox_k);

    for (int i = 0; i < n; i++) {
        tiles_touched[i + 1] += tiles_touched[i];
    }

    ///////// DUPLICATE GAUSSIANS + CALC TILE IDS

    uint32_t num_intersections = tiles_touched[n];
    init_projected_gaussian_ptr(pg_all, &data_arena, num_intersections);

    kernel_reset_unifs(&tile_k);

    for (uint32_t q = 0; q < NUM_QPUS; q++) {
        kernel_load_unif(&tile_k, q, NUM_QPUS * SIMD_WIDTH);
        kernel_load_unif(&tile_k, q, n);
        kernel_load_unif(&tile_k, q, num_intersections);
        kernel_load_unif(&tile_k, q, q);

        kernel_load_unif(&tile_k, q, WIDTH / TILE_SIZE - 1);
        kernel_load_unif(&tile_k, q, HEIGHT / TILE_SIZE - 1);

        kernel_load_unif(&tile_k, q, TO_BUS(pg->screen_x));
        kernel_load_unif(&tile_k, q, TO_BUS(pg->screen_y));
        kernel_load_unif(&tile_k, q, TO_BUS(pg->radius_id));
        kernel_load_unif(&tile_k, q, TO_BUS(pg->depth_key));

        kernel_load_unif(&tile_k, q, TO_BUS(tiles_touched));

        kernel_load_unif(&tile_k, q, TO_BUS(pg_all->depth_key + q * SIMD_WIDTH));
        // use radius as original Gaussian index bc we don't need it anymore for rendering
        kernel_load_unif(&tile_k, q, TO_BUS(pg_all->radius_id + q * SIMD_WIDTH));
    }

    t = sys_timer_get_usec();
    kernel_execute(&tile_k);
    uint32_t tile_kernel_t = sys_timer_get_usec() - t;
    DEBUG_D(tile_kernel_t);

    kernel_free(&tile_k);
}

void caches_enable(void) {
    unsigned r;
    asm volatile ("MRC p15, 0, %0, c1, c0, 0" : "=r" (r));
	r |= (1 << 12); // l1 instruction cache
	r |= (1 << 11); // branch prediction
	r |= (1 << 2); // data cache
    asm volatile ("MCR p15, 0, %0, c1, c0, 0" :: "r" (r));
}

// should we flush icache?
void caches_disable(void) {
    unsigned r;
    asm volatile ("MRC p15, 0, %0, c1, c0, 0" : "=r" (r));
    //r |= 0x1800;
	r &= ~(1 << 12); // l1 instruction cache
	r &= ~(1 << 11); // branch prediction
	r &= ~(1 << 2); // data cache
    asm volatile ("MCR p15, 0, %0, c1, c0, 0" :: "r" (r));
}

void main() {
    caches_enable();

    // const int num_gaussians = NUM_QPUS * SIMD_WIDTH * 800;
    const int num_gaussians = NUM_QPUS * SIMD_WIDTH * 1;
    const int MiB = 1024 * 1024;
    arena_init(&data_arena, MiB * 230);

    // set up kernels
    kernel_init(&project_points_k, NUM_QPUS, 35,
            project_points_cov2d_inv, sizeof(project_points_cov2d_inv));
    kernel_init(&sh_k, NUM_QPUS, 60,
            spherical_harmonics, sizeof(spherical_harmonics));
    kernel_init(&bbox_k, NUM_QPUS, 13,
            calc_bbox, sizeof(calc_bbox));
    kernel_init(&tile_k, NUM_QPUS, 14,
            calc_tile, sizeof(calc_tile));
    kernel_init(&sort_copy_k, NUM_QPUS, 22,
        sort_copy, sizeof(sort_copy));
    kernel_init(&render_k, NUM_QPUS, 16,
            render, sizeof(render));

    kernel_init(&scan_rot_k, NUM_QPUS, 4,
        scan_rot, sizeof(scan_rot));
    kernel_init(&scan_sum_k, NUM_QPUS, 5,
        scan_sum, sizeof(scan_sum));

    uint32_t t;

    // Vec3 cam_pos = { { 0.0f, 0.0f, 5.0f } };
    Vec3 cam_pos = { { 0.0f, 0.0f, -5.0f } };
    // Vec3 cam_pos = { { 0.0f, 0.0f, 100.f } };
    // Vec3 cam_target = { { 0.0f, 0.0f, 0.0f } };
    Vec3 cam_target = { { 0.0f, 0.0f, 1.0f } };
    Vec3 cam_up = { { 0.0f, 1.0f, 0.0f } };

    Camera* c = arena_alloc_align(&data_arena, sizeof(Camera), 16);
    init_camera(c, cam_pos, cam_target, cam_up, WIDTH, HEIGHT);

    GaussianPtr g;
    init_gaussian_ptr(&g, &data_arena, num_gaussians);

    ProjectedGaussianPtr pg;
    init_projected_gaussian_ptr(&pg, &data_arena, num_gaussians);

    for (int i = 0; i < num_gaussians; i++) {
        // g.pos_x[i] = rand_float(-2.0f, 2.0f);
        // g.pos_y[i] = rand_float(-1.5f, 1.5f);
        // g.pos_z[i] = rand_float(-2.0f, 2.0f);
        if (i % 3 == 0) {
            g.pos_x[i] = 1.0;
            g.pos_y[i] = 0.0;
            g.pos_z[i] = 0.0;
        } else if (i % 3 == 1) {
            g.pos_x[i] = 0.0;
            g.pos_y[i] = 1.0;
            g.pos_z[i] = 0.0;
        } else {
            g.pos_x[i] = 0.0;
            g.pos_y[i] = 0.0;
            g.pos_z[i] = 1.0;
        }
        
        // float scale_f = rand_float(-3.5f, -2.5f);
        // float scale_f = rand_float(-5.5, -4.5);
        float scale_f = -2.5;
        Vec3 scale = { { scale_f, scale_f, scale_f } };
        Vec4 rot = { { 0.0f, 0.0f, 0.0f, 1.0f } };

        Mat3 cov3d_m = compute_cov3d(scale, rot);

        g.cov3d[0][i] = cov3d_m.m[0];
        g.cov3d[1][i] = cov3d_m.m[1];
        g.cov3d[2][i] = cov3d_m.m[2];
        g.cov3d[3][i] = cov3d_m.m[4];
        g.cov3d[4][i] = cov3d_m.m[5];
        g.cov3d[5][i] = cov3d_m.m[8];

        pg.opacity[i] = rand_float(0.5f, 1.0f); // load opacity directly to projected Gaussian
        
        for (int j = 0; j < 16; j++) {
            g.sh_x[j][i] = 0.0f;
            g.sh_y[j][i] = 0.0f;
            g.sh_z[j][i] = 0.0f;
        }
        
        // float r = rand_float(0.2f, 1.0f);
        // float gr = rand_float(0.2f, 1.0f);
        // float b = rand_float(0.2f, 1.0f);
        // g.sh_x[0][i] = (r - 0.5f) / SH_C0;
        // g.sh_y[0][i] = (gr - 0.5f) / SH_C0;
        // g.sh_z[0][i] = (b - 0.5f) / SH_C0;
        if (i % 3 == 0) {
            g.sh_x[0][i] = (1.0 - 0.5f) / SH_C0;
            g.sh_y[0][i] = (0.1 - 0.5f) / SH_C0;
            g.sh_z[0][i] = (0.1 - 0.5f) / SH_C0;
        } else if (i % 3 == 1) {
            g.sh_x[0][i] = (0.1 - 0.5f) / SH_C0;
            g.sh_y[0][i] = (1.0 - 0.5f) / SH_C0;
            g.sh_z[0][i] = (0.1 - 0.5f) / SH_C0;
        } else {
            g.sh_x[0][i] = (0.1 - 0.5f) / SH_C0;
            g.sh_y[0][i] = (0.1 - 0.5f) / SH_C0;
            g.sh_z[0][i] = (1.0 - 0.5f) / SH_C0;
        }
    }

    DEBUG_D(num_gaussians);

    uart_puts("DONE LOADING GAUSSIANS\n");

    t = sys_timer_get_usec();
    precompute_gaussians_qpu(c, &g, &pg, num_gaussians);
    uint32_t qpu_t = sys_timer_get_usec() - t;
    DEBUG_D(qpu_t);

    uart_puts("COUNTING INTERSECTIONS...\n");

    // qpu
    uint32_t* tiles_touched = arena_alloc_align(&data_arena, (num_gaussians + 1) * sizeof(uint32_t), 16);
    uint32_t* gaussians_touched = arena_alloc_align(&data_arena, (NUM_TILES + 1) * sizeof(uint32_t), 16);
    memset(tiles_touched, 0, (num_gaussians + 1) * sizeof(uint32_t));
    memset(gaussians_touched, 0, (NUM_TILES + 1) * sizeof(uint32_t));

    ProjectedGaussianPtr pg_all;
    count_intersections(&pg, num_gaussians,
            tiles_touched, &pg_all);

    uint32_t total_intersections = tiles_touched[num_gaussians];
    DEBUG_D(total_intersections);
    assert(total_intersections < MAX_GAUSSIANS, "too many intersections");

    DEBUG_D(tiles_touched[num_gaussians]);


    uart_puts("SORTING...\n");

    t = sys_timer_get_usec();
    sort(&pg_all, total_intersections, &pg, gaussians_touched);
    uint32_t qpu_sort_t = sys_timer_get_usec() - t;
    DEBUG_D(qpu_sort_t);

    // rpi_reset();


    uint32_t *fb;
    uint32_t size, pitch;

    uart_puts("Initializing framebuffer...\n");
    mbox_framebuffer_init(WIDTH, HEIGHT, 32, &fb, &size, &pitch);

    DEBUG_XM(fb, "buffer ptr");
    DEBUG_D(size);
    DEBUG_D(pitch);
    DEBUG_DM(WIDTH * 4, "expected");

    if (pitch != WIDTH * 4) {
        panic("Framebuffer init failed!");
    }

    uint32_t* pixels = (uint32_t*)((uintptr_t)fb & 0x3FFFFFFF); 

    uart_puts("RENDERING\n");

    kernel_reset_unifs(&render_k);
    for (uint32_t q = 0; q < NUM_QPUS; q++) {
        kernel_load_unif(&render_k, q, NUM_QPUS);
        kernel_load_unif(&render_k, q, NUM_TILES);
        kernel_load_unif(&render_k, q, q);

        kernel_load_unif(&render_k, q, WIDTH / TILE_SIZE);
        kernel_load_unif(&render_k, q, 1.0 * TILE_SIZE / WIDTH);

        kernel_load_unif(&render_k, q, TO_BUS(pg_all.cov2d_inv_x));
        kernel_load_unif(&render_k, q, TO_BUS(pg_all.cov2d_inv_y));
        kernel_load_unif(&render_k, q, TO_BUS(pg_all.cov2d_inv_z));
        kernel_load_unif(&render_k, q, TO_BUS(pg_all.opacity));
        kernel_load_unif(&render_k, q, TO_BUS(pg_all.screen_x));
        kernel_load_unif(&render_k, q, TO_BUS(pg_all.screen_y));
        kernel_load_unif(&render_k, q, TO_BUS(pg_all.color_r));
        kernel_load_unif(&render_k, q, TO_BUS(pg_all.color_g));
        kernel_load_unif(&render_k, q, TO_BUS(pg_all.color_b));

        kernel_load_unif(&render_k, q, TO_BUS(gaussians_touched));
        kernel_load_unif(&render_k, q, TO_BUS(pixels));
    }

    t = sys_timer_get_usec();
    kernel_execute(&render_k);
    uint32_t render_t = sys_timer_get_usec() - t;
    DEBUG_D(render_t);

    uart_puts("DONE RENDERING\n");

    float MB_used = 1.0 * data_arena.size / MiB;
    DEBUG_F(MB_used);

    caches_disable();
    while (1);

    rpi_reset();
}
