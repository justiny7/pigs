#include "gpio.h"
#include "uart.h"
#include "lib.h"
#include "sys_timer.h"
#include "mailbox_interface.h"
#include "gaussian.h"
#include "math.h"
#include "debug.h"
#include "kernel.h"
#include "emmc.h"
#include "fat.h"

#include "project_points_cov2d_inv.h"
#include "spherical_harmonics.h"
#include "calc_bbox.h"
#include "calc_tile.h"
#include "sort_copy.h"
#include "render.h"
#include "scan_rot.h"
#include "scan_sum.h"

#define WIDTH 1280
#define HEIGHT 720
// #define WIDTH 640
// #define HEIGHT 480
// #define WIDTH 256
// #define HEIGHT 192
// #define WIDTH 128
// #define HEIGHT 96
#define TILE_SIZE 16
#define NUM_TILES (WIDTH * HEIGHT / (TILE_SIZE * TILE_SIZE))

#define NUM_QPUS 12
#define SIMD_WIDTH 16

#define FILE_NAME "FLY     PLY"
// #define FILE_NAME "CACTUS  PLY"

Arena data_arena;
Kernel project_points_k, sh_k, bbox_k, tile_k, sort_copy_k, render_k;
Kernel scan_rot_k, scan_sum_k;

GaussianPtr g;
ProjectedGaussianPtr pg, pg_all;


void init_kernels() {
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
}

void free_kernels() {
    kernel_free(&project_points_k);
    kernel_free(&sh_k);
    kernel_free(&bbox_k);
    kernel_free(&tile_k);
    kernel_free(&sort_copy_k);
    kernel_free(&render_k);
    kernel_free(&scan_rot_k);
    kernel_free(&scan_sum_k);
}

void qpu_scan(uint32_t* arr, int n) {
    assert((n & 0xF) == 0, "N must be divisble by 16 for scan");
    assert(n >= NUM_QPUS * SIMD_WIDTH, "N too small for QPU scan");

    uint32_t arena_size = data_arena.size;

    kernel_reset_unifs(&scan_rot_k);

    for (int q = 0; q < NUM_QPUS; q++) {
        kernel_load_unif(&scan_rot_k, q, NUM_QPUS * SIMD_WIDTH);
        kernel_load_unif(&scan_rot_k, q, n);
        kernel_load_unif(&scan_rot_k, q, q);

        kernel_load_unif(&scan_rot_k, q, TO_BUS(arr + q * SIMD_WIDTH));
    }

    kernel_execute(&scan_rot_k);

    uint32_t* pref = arena_alloc_align(&data_arena, n * sizeof(uint32_t), 16 * sizeof(uint32_t));
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

    arena_dealloc_to(&data_arena, arena_size);
}

void sort(ProjectedGaussianPtr* pg, int n, ProjectedGaussianPtr* orig, uint32_t* gaussians_touched) {
    uint32_t t;
    uint32_t arena_size = data_arena.size;

    // key = tile (12 bits) | ~(upper 20 bits of depth)
    // depth bits are flipped bc we want depth descending
    // since we only have 12 bits for tile, we can only have max 4096 tiles

    uint32_t* temp_key = arena_alloc_align(&data_arena, n * sizeof(uint32_t), 16 * sizeof(uint32_t));
    uint32_t* temp_id = arena_alloc_align(&data_arena, n * sizeof(uint32_t), 16 * sizeof(uint32_t));

    uint32_t* cnt = arena_alloc_align(&data_arena, (1 << 16) * sizeof(uint32_t), 16 * sizeof(uint32_t));
    uint32_t* cnt2 = arena_alloc_align(&data_arena, (1 << 16) * sizeof(uint32_t), 16 * sizeof(uint32_t));
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
            temp_key[--cnt[j]] = (key >>= 16);
            temp_id[cnt[j]] = pg->radius_id[i].id;
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
    if (NUM_TILES + 1 < NUM_QPUS * SIMD_WIDTH) {
        for (int i = 0; i < NUM_TILES; i++) {
            gaussians_touched[i + 1] += gaussians_touched[i];
        }
    } else {
        qpu_scan(gaussians_touched, (NUM_TILES + 1 + 15) & ~0xF);
    }

    uint32_t prefsum_tot = sys_timer_get_usec() - t;
    DEBUG_D(prefsum_tot);

    arena_dealloc_to(&data_arena, arena_size);
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

    ////////// SPHERICAL HARMONICS
    kernel_reset_unifs(&sh_k);
    float** sh[3] = { g->sh_x, g->sh_y, g->sh_z };
    float* colors[3] = { pg->color_r, pg->color_g, pg->color_b };

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

    qpu_scan(tiles_touched, (n + 1 + 15) & ~0xF);

    ///////// DUPLICATE GAUSSIANS + CALC TILE IDS

    uint32_t num_intersections = tiles_touched[n];
    DEBUG_D(num_intersections);
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

void init_sd(char** data_ptr, uint32_t* filesize_ptr) {
    if (sd_init() != SD_OK) {
        uart_puts("ERROR: SD init failed\n");
        rpi_reset();
    }
    uart_puts("SD init OK\n");

    if (!fat_getpartition()) {
        uart_puts("ERROR: FAT partition not found\n");
        rpi_reset();
    }
    uart_puts("FAT partition OK\n");

    unsigned int file_size = 0;
    unsigned int cluster = fat_getcluster(FILE_NAME, &file_size);
    if (cluster == 0) {
        uart_puts("ERROR: File not found: " FILE_NAME "\n");
        rpi_reset();
    }

    unsigned int bytes_read = 0;
    char* data = fat_readfile(cluster, &bytes_read);
    if (data == 0) {
        uart_puts("ERROR: Failed to read file\n");
        rpi_reset();
    }

    uart_puts("File size: ");
    uart_putd(file_size);
    uart_puts(" bytes, read: ");
    uart_putd(bytes_read);
    uart_puts(" bytes\n");

    *data_ptr = data;
    *filesize_ptr = file_size;
}

void read_ply(uint32_t* num_gaussians_ptr,
        float* x_avg_ptr, float* y_avg_ptr, float* z_avg_ptr) {

    char* data;
    uint32_t filesize;
    init_sd(&data, &filesize);

    const char* vertex_count = "vertex ";
    const char* end_header = "end_header\n";

    int st = 0;
    for (; ; st++) {
        int f = 0;
        for (int j = 0; j < 7; j++) {
            if (data[st + j] != vertex_count[j]) {
                f = 1;
                break;
            }
        }

        if (!f) {
            break;
        }
    }
    st += 7;

    uint32_t num_gaussians = 0;
    while (data[st] != '\n') {
        num_gaussians = num_gaussians * 10 + (data[st++] - '0');
    }

    init_gaussian_ptr(&g, &data_arena, num_gaussians);
    init_projected_gaussian_ptr(&pg, &data_arena, num_gaussians);

    for (; ; st++) {
        int f = 0;
        for (int j = 0; j < 11; j++) {
            if (data[st + j] != end_header[j]) {
                f = 1;
                break;
            }
        }

        if (!f) {
            break;
        }
    }

    st += 11;
    assert(st + num_gaussians * sizeof(Gaussian) == filesize, "ply file size mismatch");

    float x_avg = 0.0f, y_avg = 0.0f, z_avg = 0.0f;
    for (uint32_t i = 0; i < num_gaussians; i++) {
        Gaussian gaus;
        memcpy(&gaus, data + st, sizeof(Gaussian));

        g.pos_x[i] = gaus.pos_x;
        g.pos_y[i] = gaus.pos_y;
        g.pos_z[i] = gaus.pos_z;

        x_avg += g.pos_x[i];
        y_avg += g.pos_y[i];
        z_avg += g.pos_z[i];

        g.sh_x[0][i] = gaus.f_dc[0];
        g.sh_y[0][i] = gaus.f_dc[1];
        g.sh_z[0][i] = gaus.f_dc[2];
        for (int j = 0; j < 15; j++) {
            g.sh_x[j + 1][i] = gaus.f_rest[j];
            g.sh_y[j + 1][i] = gaus.f_rest[j + 15];
            g.sh_z[j + 1][i] = gaus.f_rest[j + 30];
        }

        pg.opacity[i] = 1.0 / (1.0 + expf(-gaus.opacity));

        Vec3 scale = { { gaus.scale_x, gaus.scale_y, gaus.scale_z } };
        Vec4 rot = { { gaus.rot_x, gaus.rot_y, gaus.rot_z, gaus.rot_w } };
        assert((1.0 - vec4_len(rot)) < 0.001f, "rot not normalized");

        Mat3 cov3d_m = compute_cov3d(scale, rot);
        g.cov3d[0][i] = cov3d_m.m[0];
        g.cov3d[1][i] = cov3d_m.m[1];
        g.cov3d[2][i] = cov3d_m.m[2];
        g.cov3d[3][i] = cov3d_m.m[4];
        g.cov3d[4][i] = cov3d_m.m[5];
        g.cov3d[5][i] = cov3d_m.m[8];

        st += sizeof(Gaussian);

        if ((i + 1) % 5000 == 0) {
            uart_puts("loaded ");
            uart_putd(i + 1);
            uart_puts(" gaussians\n");
        }
    }

    *num_gaussians_ptr = num_gaussians;
    *x_avg_ptr = x_avg / num_gaussians;
    *y_avg_ptr = y_avg / num_gaussians;
    *z_avg_ptr = z_avg / num_gaussians;
}

void render_gaussians(Camera* c, uint32_t num_gaussians) {
    uint32_t t;

    t = sys_timer_get_usec();
    precompute_gaussians_qpu(c, &g, &pg, num_gaussians);
    uint32_t qpu_t = sys_timer_get_usec() - t;
    DEBUG_D(qpu_t);

    uart_puts("COUNTING INTERSECTIONS...\n");

    // qpu
    uint32_t* tiles_touched = arena_alloc_align(&data_arena, (num_gaussians + 1) * sizeof(uint32_t), 16 * sizeof(uint32_t));
    uint32_t* gaussians_touched = arena_alloc_align(&data_arena, (NUM_TILES + 1) * sizeof(uint32_t), 16 * sizeof(uint32_t));
    memset(tiles_touched, 0, (num_gaussians + 1) * sizeof(uint32_t));
    memset(gaussians_touched, 0, (NUM_TILES + 1) * sizeof(uint32_t));

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
    for (uint32_t q = 0; q < render_k.num_qpus; q++) {
        kernel_load_unif(&render_k, q, render_k.num_qpus);
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

}

void main() {
    caches_enable();

    const int MiB = 1024 * 1024;
    arena_init(&data_arena, MiB * 230);

    init_kernels();

    uint32_t num_gaussians;
    float x_avg, y_avg, z_avg;
    read_ply(&num_gaussians, &x_avg, &y_avg, &z_avg);

    // FLY
    // Vec3 cam_pos = { { x_avg - 0.1, y_avg, z_avg + 0.3 } };
    Vec3 cam_pos = { { x_avg - 0.25, y_avg - 0.1, z_avg - 0.25 } };
    // Vec3 cam_pos = { { x_avg - 0.2, y_avg, z_avg + 0.5 } };
    // Vec3 cam_pos = { { x_avg - 0.5, y_avg, z_avg  } };

    // CACTUS
    // Vec3 cam_pos = { { 0.0f, -1.5f, 2.5f } };

    Vec3 cam_target = { { x_avg, y_avg, z_avg} };
    Vec3 cam_up = { { 0.0f, 1.0f, 0.0f } };

    Camera* c = arena_alloc_align(&data_arena, sizeof(Camera), 16);
    init_camera(c, cam_pos, cam_target, cam_up, WIDTH, HEIGHT);

    DEBUG_D(num_gaussians);

    uart_puts("DONE LOADING GAUSSIANS\n");

    render_gaussians(c, num_gaussians);

    free_kernels();

    float MB_used = 1.0 * data_arena.size / MiB;
    DEBUG_F(MB_used);

    caches_disable();
    while (1);

    rpi_reset();
}
