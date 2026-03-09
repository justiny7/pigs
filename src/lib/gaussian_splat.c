#include "gaussian_splat.h"
#include "fat.h"
#include "emmc.h"
#include "sys_timer.h"
#include "lib.h"
#include "math.h"
#include "uart.h"

#include "project_points_cov2d_inv.h"
#include "spherical_harmonics.h"
#include "calc_bbox.h"
#include "calc_tile.h"
#include "sort_copy.h"
#include "render.h"
#include "scan_rot.h"
#include "scan_sum.h"

// #define DEBUG
#include "debug.h"

static Kernel project_points_k, sh_k, bbox_k, tile_k, sort_copy_k, render_k;
static Kernel scan_rot_k, scan_sum_k;

// #define VERBOSE

void gs_init(GaussianSplat* gs, Arena* data_arena, uint32_t* framebuffer, uint32_t num_qpus) {
    caches_enable();

    kernel_init(&project_points_k, num_qpus, 35,
            project_points_cov2d_inv, sizeof(project_points_cov2d_inv));
    kernel_init(&sh_k, num_qpus, 60,
            spherical_harmonics, sizeof(spherical_harmonics));
    kernel_init(&bbox_k, num_qpus, 13,
            calc_bbox, sizeof(calc_bbox));
    kernel_init(&tile_k, num_qpus, 14,
            calc_tile, sizeof(calc_tile));
    kernel_init(&sort_copy_k, num_qpus, 22,
            sort_copy, sizeof(sort_copy));
    kernel_init(&render_k, num_qpus, 16,
            render, sizeof(render));

    kernel_init(&scan_rot_k, num_qpus, 4,
            scan_rot, sizeof(scan_rot));
    kernel_init(&scan_sum_k, num_qpus, 5,
            scan_sum, sizeof(scan_sum));

    gs->data_arena = data_arena;
    gs->num_qpus = num_qpus;
    gs->framebuffer = framebuffer;
}
void gs_set_camera(GaussianSplat* gs, Camera* c) {
    assert((c->width & 0xF) == 0, "Width must be divisble by 16");
    assert((c->height & 0xF) == 0, "Height must be divisble by 16");

    gs->c = c;
    gs->num_tiles = c->width * c->height / (TILE_SIZE * TILE_SIZE);
}
void gs_free(GaussianSplat* gs) {
    kernel_free(&project_points_k);
    kernel_free(&sh_k);
    kernel_free(&bbox_k);
    kernel_free(&tile_k);
    kernel_free(&sort_copy_k);
    kernel_free(&render_k);
    kernel_free(&scan_rot_k);
    kernel_free(&scan_sum_k);
}

void init_sd(const char* filename, char** data_ptr, uint32_t* filesize_ptr) {
    if (sd_init() != SD_OK) {
        uart_puts("ERROR: SD init failed\n");
        rpi_reset();
    }
#ifdef VERBOSE
    uart_puts("SD init OK\n");
#endif

    if (!fat_getpartition()) {
        uart_puts("ERROR: FAT partition not found\n");
        rpi_reset();
    }
#ifdef VERBOSE
    uart_puts("FAT partition OK\n");
#endif

    unsigned int file_size = 0;
    unsigned int cluster = fat_getcluster(filename, &file_size);
    if (cluster == 0) {
        uart_puts("ERROR: File not found: ");
        uart_puts(filename);
        uart_puts("\n");
        rpi_reset();
    }

    unsigned int bytes_read = 0;
    char* data = fat_readfile(cluster, &bytes_read);
    if (data == 0) {
        uart_puts("ERROR: Failed to read file\n");
        rpi_reset();
    }

#ifdef VERBOSE
    uart_puts("File size: ");
    uart_putd(file_size);
    uart_puts(" bytes, read: ");
    uart_putd(bytes_read);
    uart_puts(" bytes\n");
#endif

    *data_ptr = data;
    *filesize_ptr = file_size;
}

void gs_read_ply(GaussianSplat* gs, const char* filename,
        float* x_avg, float* y_avg, float* z_avg) {

    char* data;
    uint32_t filesize;
    init_sd(filename, &data, &filesize);

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

    gs->num_gaussians = num_gaussians;
    init_gaussian_ptr(&gs->g, gs->data_arena, num_gaussians);
    init_projected_gaussian_ptr(&gs->pg, gs->data_arena, num_gaussians);

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

    float x_sum = 0.0f, y_sum = 0.0f, z_sum = 0.0f;
    for (uint32_t i = 0; i < num_gaussians; i++) {
        Gaussian gaus;
        memcpy(&gaus, data + st, sizeof(Gaussian));

        gs->g.pos_x[i] = gaus.pos_x;
        gs->g.pos_y[i] = gaus.pos_y;
        gs->g.pos_z[i] = gaus.pos_z;

        x_sum += gs->g.pos_x[i];
        y_sum += gs->g.pos_y[i];
        z_sum += gs->g.pos_z[i];

        gs->g.sh_x[0][i] = gaus.f_dc[0];
        gs->g.sh_y[0][i] = gaus.f_dc[1];
        gs->g.sh_z[0][i] = gaus.f_dc[2];
        for (int j = 0; j < 15; j++) {
            gs->g.sh_x[j + 1][i] = gaus.f_rest[j];
            gs->g.sh_y[j + 1][i] = gaus.f_rest[j + 15];
            gs->g.sh_z[j + 1][i] = gaus.f_rest[j + 30];
        }

        gs->pg.opacity[i] = 1.0 / (1.0 + expf(-gaus.opacity));

        Vec3 scale = { { gaus.scale_x, gaus.scale_y, gaus.scale_z } };
        Vec4 rot = { { gaus.rot_x, gaus.rot_y, gaus.rot_z, gaus.rot_w } };
        rot = vec4_sdiv(rot, vec4_len(rot));
        assert((1.0 - vec4_len(rot)) < 0.001f, "rot not normalized");

        Mat3 cov3d_m = compute_cov3d(scale, rot);
        gs->g.cov3d[0][i] = cov3d_m.m[0];
        gs->g.cov3d[1][i] = cov3d_m.m[1];
        gs->g.cov3d[2][i] = cov3d_m.m[2];
        gs->g.cov3d[3][i] = cov3d_m.m[4];
        gs->g.cov3d[4][i] = cov3d_m.m[5];
        gs->g.cov3d[5][i] = cov3d_m.m[8];

        st += sizeof(Gaussian);

        if ((i + 1) % 5000 == 0) {
            uart_puts("Loaded ");
            uart_putd(i + 1);
            uart_puts(" gaussians\n");
        }
    }

    *x_avg = x_sum / num_gaussians;
    *y_avg = y_sum / num_gaussians;
    *z_avg = z_sum / num_gaussians;
}

void qpu_scan(GaussianSplat* gs, uint32_t* arr, uint32_t n) {
    assert((n & 0xF) == 0, "N must be divisble by 16 for scan");
    assert(n >= gs->num_qpus * SIMD_WIDTH, "N too small for QPU scan");

    uint32_t arena_size = gs->data_arena->size;

    kernel_reset_unifs(&scan_rot_k);

    for (uint32_t q = 0; q < gs->num_qpus; q++) {
        kernel_load_unif(&scan_rot_k, q, gs->num_qpus * SIMD_WIDTH);
        kernel_load_unif(&scan_rot_k, q, n);
        kernel_load_unif(&scan_rot_k, q, q);

        kernel_load_unif(&scan_rot_k, q, TO_BUS(arr + q * SIMD_WIDTH));
    }

    kernel_execute(&scan_rot_k);

    uint32_t* pref = arena_alloc_align(gs->data_arena, n * sizeof(uint32_t), 16 * sizeof(uint32_t));
    pref[0] = 0;
    for (uint32_t i = 0; i + SIMD_WIDTH < n; i += SIMD_WIDTH) {
        pref[i + SIMD_WIDTH] = pref[i] + arr[i + SIMD_WIDTH - 1];
    }

    kernel_reset_unifs(&scan_sum_k);

    for (uint32_t q = 0; q < gs->num_qpus; q++) {
        kernel_load_unif(&scan_sum_k, q, gs->num_qpus * SIMD_WIDTH);
        kernel_load_unif(&scan_sum_k, q, n);
        kernel_load_unif(&scan_sum_k, q, q);

        kernel_load_unif(&scan_sum_k, q, TO_BUS(arr + q * SIMD_WIDTH));
        kernel_load_unif(&scan_sum_k, q, TO_BUS(pref + q * SIMD_WIDTH));
    }

    kernel_execute(&scan_sum_k);

    arena_dealloc_to(gs->data_arena, arena_size);
}

void sort(GaussianSplat* gs, uint32_t* gaussians_touched) {
    uint32_t t;
    uint32_t arena_size = gs->data_arena->size;

    // key = tile (12 bits) | ~(upper 20 bits of depth)
    // depth bits are flipped bc we want depth descending
    // since we only have 12 bits for tile, we can only have max 4096 tiles

    uint32_t* temp_key = arena_alloc_align(gs->data_arena, gs->num_intersections * sizeof(uint32_t), 16 * sizeof(uint32_t));
    uint32_t* temp_id = arena_alloc_align(gs->data_arena, gs->num_intersections * sizeof(uint32_t), 16 * sizeof(uint32_t));

    uint32_t* cnt = arena_alloc_align(gs->data_arena, (1 << 16) * sizeof(uint32_t), 16 * sizeof(uint32_t));
    uint32_t* cnt2 = arena_alloc_align(gs->data_arena, (1 << 16) * sizeof(uint32_t), 16 * sizeof(uint32_t));
    memset(cnt, 0, (1 << 16) * sizeof(uint32_t));
    memset(cnt2, 0, (1 << 16) * sizeof(uint32_t));

    t = sys_timer_get_usec();
    {   // pass 1
        for (uint32_t i = 0; i < gs->num_intersections; i++) {
            uint32_t key = gs->pg_all.depth_key[i].key;

            cnt[key & 0xFFFF]++;
            gaussians_touched[(key >> 20) + 1]++;
        }

        qpu_scan(gs, cnt, 1 << 16);

        for (int i = gs->num_intersections - 1; i >= 0; i--) {
            uint32_t key = gs->pg_all.depth_key[i].key;
            int j = key & 0xFFFF;
            temp_key[--cnt[j]] = (key >>= 16);
            temp_id[cnt[j]] = gs->pg_all.radius_id[i].id;
            cnt2[key]++;
        }
    }
    uint32_t iter1_tot = sys_timer_get_usec() - t;
    DEBUG_D(iter1_tot);

    t = sys_timer_get_usec();
    {   // pass 2: no need to copy key
        qpu_scan(gs, cnt2, 1 << 16);

        for (int i = gs->num_intersections - 1; i >= 0; i--) {
            gs->pg_all.radius_id[--cnt2[temp_key[i]]].id = temp_id[i];
        }

        kernel_reset_unifs(&sort_copy_k);

        for (uint32_t q = 0; q < gs->num_qpus; q++) {
            kernel_load_unif(&sort_copy_k, q, gs->num_qpus * SIMD_WIDTH);
            kernel_load_unif(&sort_copy_k, q, gs->num_intersections);
            kernel_load_unif(&sort_copy_k, q, q);

            kernel_load_unif(&sort_copy_k, q, TO_BUS(gs->pg_all.radius_id + q * SIMD_WIDTH));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(gs->pg_all.screen_x + q * SIMD_WIDTH));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(gs->pg_all.screen_y + q * SIMD_WIDTH));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(gs->pg_all.cov2d_inv_x + q * SIMD_WIDTH));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(gs->pg_all.cov2d_inv_y + q * SIMD_WIDTH));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(gs->pg_all.cov2d_inv_z + q * SIMD_WIDTH));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(gs->pg_all.color_r + q * SIMD_WIDTH));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(gs->pg_all.color_g + q * SIMD_WIDTH));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(gs->pg_all.color_b + q * SIMD_WIDTH));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(gs->pg_all.opacity + q * SIMD_WIDTH));

            kernel_load_unif(&sort_copy_k, q, TO_BUS(gs->pg.screen_x));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(gs->pg.screen_y));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(gs->pg.cov2d_inv_x));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(gs->pg.cov2d_inv_y));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(gs->pg.cov2d_inv_z));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(gs->pg.color_r));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(gs->pg.color_g));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(gs->pg.color_b));
            kernel_load_unif(&sort_copy_k, q, TO_BUS(gs->pg.opacity));
        }

        kernel_execute(&sort_copy_k);
    }
    uint32_t iter2_tot = sys_timer_get_usec() - t;
    DEBUG_D(iter2_tot);
    t = sys_timer_get_usec();

    if (gs->num_tiles + 1 < gs->num_qpus * SIMD_WIDTH) {
        for (uint32_t i = 0; i < gs->num_tiles; i++) {
            gaussians_touched[i + 1] += gaussians_touched[i];
        }
    } else {
        qpu_scan(gs, gaussians_touched, (gs->num_tiles + 1 + 15) & ~0xF);
    }

    uint32_t prefsum_tot = sys_timer_get_usec() - t;
    DEBUG_D(prefsum_tot);

    arena_dealloc_to(gs->data_arena, arena_size);
}

void precompute_gaussians_qpu(GaussianSplat* gs) {
    //////// PROJECT POINTS + COV2D INV
    kernel_reset_unifs(&project_points_k);

    for (uint32_t q = 0; q < gs->num_qpus; q++) {
        kernel_load_unif(&project_points_k, q, gs->num_qpus * SIMD_WIDTH);
        kernel_load_unif(&project_points_k, q, gs->num_gaussians);
        kernel_load_unif(&project_points_k, q, q);
        kernel_load_unif(&project_points_k, q, TO_BUS(gs->g.pos_x + q * SIMD_WIDTH));
        kernel_load_unif(&project_points_k, q, TO_BUS(gs->g.pos_y + q * SIMD_WIDTH));
        kernel_load_unif(&project_points_k, q, TO_BUS(gs->g.pos_z + q * SIMD_WIDTH));
        kernel_load_unif(&project_points_k, q, TO_BUS(gs->pg.depth_key + q * SIMD_WIDTH));
        kernel_load_unif(&project_points_k, q, TO_BUS(gs->pg.screen_x + q * SIMD_WIDTH));
        kernel_load_unif(&project_points_k, q, TO_BUS(gs->pg.screen_y + q * SIMD_WIDTH));
        kernel_load_unif(&project_points_k, q, TO_BUS(gs->pg.radius_id + q * SIMD_WIDTH));

        //  in order that we need for computation
        for (uint32_t i = 0; i < 12; i++) {
            kernel_load_unif(&project_points_k, q, gs->c->w2c.m[i]);
        }
        kernel_load_unif(&project_points_k, q, gs->c->fx);
        kernel_load_unif(&project_points_k, q, gs->c->cx);
        kernel_load_unif(&project_points_k, q, gs->c->fy);
        kernel_load_unif(&project_points_k, q, gs->c->cy);

        for (uint32_t i = 0; i < 6; i++) {
            kernel_load_unif(&project_points_k, q, TO_BUS(gs->g.cov3d[i] + q * SIMD_WIDTH));
        }

        kernel_load_unif(&project_points_k, q, TO_BUS(gs->pg.cov2d_inv_x + q * SIMD_WIDTH));
        kernel_load_unif(&project_points_k, q, TO_BUS(gs->pg.cov2d_inv_y + q * SIMD_WIDTH));
        kernel_load_unif(&project_points_k, q, TO_BUS(gs->pg.cov2d_inv_z + q * SIMD_WIDTH));
    }

    uint32_t t;

    t = sys_timer_get_usec();
    kernel_execute(&project_points_k);

    uint32_t project_points_kernel_t  = sys_timer_get_usec() - t;
    DEBUG_D(project_points_kernel_t);

    ////////// SPHERICAL HARMONICS
    kernel_reset_unifs(&sh_k);
    float** sh[3] = { gs->g.sh_x, gs->g.sh_y, gs->g.sh_z };
    float* colors[3] = { gs->pg.color_r, gs->pg.color_g, gs->pg.color_b };

    for (uint32_t q = 0; q < gs->num_qpus; q++) {
        kernel_load_unif(&sh_k, q, gs->num_qpus * SIMD_WIDTH);
        kernel_load_unif(&sh_k, q, gs->num_gaussians);
        kernel_load_unif(&sh_k, q, q);

        kernel_load_unif(&sh_k, q, TO_BUS(gs->g.pos_x + q * SIMD_WIDTH));
        kernel_load_unif(&sh_k, q, TO_BUS(gs->g.pos_y + q * SIMD_WIDTH));
        kernel_load_unif(&sh_k, q, TO_BUS(gs->g.pos_z + q * SIMD_WIDTH));

        kernel_load_unif(&sh_k, q, gs->c->pos.x);
        kernel_load_unif(&sh_k, q, gs->c->pos.y);
        kernel_load_unif(&sh_k, q, gs->c->pos.z);

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

void count_intersections(GaussianSplat* gs, uint32_t* tiles_touched) {
    uint32_t t;

    /////////// CALCULATE BBOX
    kernel_reset_unifs(&bbox_k);

    for (uint32_t q = 0; q < gs->num_qpus; q++) {
        kernel_load_unif(&bbox_k, q, gs->num_qpus * SIMD_WIDTH);
        kernel_load_unif(&bbox_k, q, gs->num_gaussians);
        kernel_load_unif(&bbox_k, q, q);

        kernel_load_unif(&bbox_k, q, gs->c->width / TILE_SIZE - 1);
        kernel_load_unif(&bbox_k, q, gs->c->height / TILE_SIZE - 1);

        kernel_load_unif(&bbox_k, q, TO_BUS(gs->pg.screen_x + q * SIMD_WIDTH));
        kernel_load_unif(&bbox_k, q, TO_BUS(gs->pg.screen_y + q * SIMD_WIDTH));
        kernel_load_unif(&bbox_k, q, TO_BUS(gs->pg.radius_id + q * SIMD_WIDTH));

        kernel_load_unif(&bbox_k, q, TO_BUS(tiles_touched + 1 + q * SIMD_WIDTH));
    }

    t = sys_timer_get_usec();
    kernel_execute(&bbox_k);

    uint32_t bbox_kernel_t = sys_timer_get_usec() - t;
    DEBUG_D(bbox_kernel_t);

    qpu_scan(gs, tiles_touched, (gs->num_gaussians + 1 + 15) & ~0xF);

    ///////// DUPLICATE GAUSSIANS + CALC TILE IDS

    gs->num_intersections = tiles_touched[gs->num_gaussians];
    init_projected_gaussian_ptr(&gs->pg_all, gs->data_arena, gs->num_intersections);
    DEBUG_D(gs->num_intersections);

    kernel_reset_unifs(&tile_k);

    for (uint32_t q = 0; q < gs->num_qpus; q++) {
        kernel_load_unif(&tile_k, q, gs->num_qpus * SIMD_WIDTH);
        kernel_load_unif(&tile_k, q, gs->num_gaussians);
        kernel_load_unif(&tile_k, q, gs->num_intersections);
        kernel_load_unif(&tile_k, q, q);

        kernel_load_unif(&tile_k, q, gs->c->width / TILE_SIZE - 1);
        kernel_load_unif(&tile_k, q, gs->c->height / TILE_SIZE - 1);

        kernel_load_unif(&tile_k, q, TO_BUS(gs->pg.screen_x));
        kernel_load_unif(&tile_k, q, TO_BUS(gs->pg.screen_y));
        kernel_load_unif(&tile_k, q, TO_BUS(gs->pg.radius_id));
        kernel_load_unif(&tile_k, q, TO_BUS(gs->pg.depth_key));

        kernel_load_unif(&tile_k, q, TO_BUS(tiles_touched));

        kernel_load_unif(&tile_k, q, TO_BUS(gs->pg_all.depth_key + q * SIMD_WIDTH));
        // use radius as original Gaussian index bc we don't need it anymore for rendering
        kernel_load_unif(&tile_k, q, TO_BUS(gs->pg_all.radius_id + q * SIMD_WIDTH));
    }

    t = sys_timer_get_usec();
    kernel_execute(&tile_k);

    uint32_t tile_kernel_t = sys_timer_get_usec() - t;
    DEBUG_D(tile_kernel_t);
}

void gs_render(GaussianSplat* gs) {
    uint32_t t;

    uint32_t arena_size = gs->data_arena->size;

    t = sys_timer_get_usec();
    precompute_gaussians_qpu(gs);
    uint32_t qpu_t = sys_timer_get_usec() - t;

    DEBUG_D(qpu_t);

#ifdef VERBOSE
    uart_puts("COUNTING INTERSECTIONS...\n");
#endif

    uint32_t* tiles_touched = arena_alloc_align(gs->data_arena, (gs->num_gaussians + 1) * sizeof(uint32_t), 16 * sizeof(uint32_t));
    uint32_t* gaussians_touched = arena_alloc_align(gs->data_arena, (gs->num_tiles + 1) * sizeof(uint32_t), 16 * sizeof(uint32_t));
    memset(tiles_touched, 0, (gs->num_gaussians + 1) * sizeof(uint32_t));
    memset(gaussians_touched, 0, (gs->num_tiles + 1) * sizeof(uint32_t));

    count_intersections(gs, tiles_touched);

    uint32_t total_intersections = tiles_touched[gs->num_gaussians];
    assert(total_intersections < MAX_GAUSSIANS, "too many intersections");

    DEBUG_D(total_intersections);
    DEBUG_D(tiles_touched[gs->num_gaussians]);

#ifdef VERBOSE
    uart_puts("SORTING...\n");
#endif

    t = sys_timer_get_usec();
    sort(gs, gaussians_touched);
    uint32_t qpu_sort_t = sys_timer_get_usec() - t;
    DEBUG_D(qpu_sort_t);

#ifdef VERBOSE
    uart_puts("RENDERING\n");
#endif

    kernel_reset_unifs(&render_k);
    for (uint32_t q = 0; q < render_k.num_qpus; q++) {
        kernel_load_unif(&render_k, q, render_k.num_qpus);
        kernel_load_unif(&render_k, q, gs->num_tiles);
        kernel_load_unif(&render_k, q, q);

        kernel_load_unif(&render_k, q, gs->c->width / TILE_SIZE);
        kernel_load_unif(&render_k, q, 1.0 * TILE_SIZE / gs->c->width);

        kernel_load_unif(&render_k, q, TO_BUS(gs->pg_all.cov2d_inv_x));
        kernel_load_unif(&render_k, q, TO_BUS(gs->pg_all.cov2d_inv_y));
        kernel_load_unif(&render_k, q, TO_BUS(gs->pg_all.cov2d_inv_z));
        kernel_load_unif(&render_k, q, TO_BUS(gs->pg_all.opacity));
        kernel_load_unif(&render_k, q, TO_BUS(gs->pg_all.screen_x));
        kernel_load_unif(&render_k, q, TO_BUS(gs->pg_all.screen_y));
        kernel_load_unif(&render_k, q, TO_BUS(gs->pg_all.color_r));
        kernel_load_unif(&render_k, q, TO_BUS(gs->pg_all.color_g));
        kernel_load_unif(&render_k, q, TO_BUS(gs->pg_all.color_b));

        kernel_load_unif(&render_k, q, TO_BUS(gaussians_touched));
        kernel_load_unif(&render_k, q, TO_BUS(gs->framebuffer));
    }

    t = sys_timer_get_usec();
    kernel_execute(&render_k);
    uint32_t render_t = sys_timer_get_usec() - t;
    DEBUG_D(render_t);

#ifdef VERBOSE
    uart_puts("DONE RENDERING\n");
#endif

    arena_dealloc_to(gs->data_arena, arena_size);
}
