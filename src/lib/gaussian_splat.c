#include "gaussian_splat.h"
#include "fat.h"
#include "emmc.h"
#include "sys_timer.h"
#include "lib.h"
#include "math.h"
#include "uart.h"
#include "mmu.h"
#include "mailbox_interface.h"

#include "project_points_cov2d_inv.h"
#include "spherical_harmonics.h"
#include "calc_bbox.h"
#include "calc_tile.h"
#include "render.h"
#include "scan_rot.h"
#include "scan_sum.h"

// #define DEBUG
#include "debug.h"

static Kernel project_points_k, sh_k, bbox_k, tile_k, render_k;
static Kernel scan_rot_k, scan_sum_k;

// #define VERBOSE

void gs_init(GaussianSplat* gs, Arena* data_arena, uint32_t* framebuffer, uint32_t num_qpus) {
    kernel_init(&project_points_k, num_qpus, 35,
            project_points_cov2d_inv, sizeof(project_points_cov2d_inv));
    kernel_init(&sh_k, num_qpus, 60,
            spherical_harmonics, sizeof(spherical_harmonics));
    kernel_init(&bbox_k, num_qpus, 13,
            calc_bbox, sizeof(calc_bbox));
    kernel_init(&tile_k, num_qpus, 14,
            calc_tile, sizeof(calc_tile));
    kernel_init(&render_k, num_qpus, 16,
            render, sizeof(render));

    kernel_init(&scan_rot_k, num_qpus, 4,
            scan_rot, sizeof(scan_rot));
    kernel_init(&scan_sum_k, num_qpus, 5,
            scan_sum, sizeof(scan_sum));

    gs->data_arena = data_arena;
    gs->num_qpus = num_qpus;
    gs->framebuffer = framebuffer;

    gs_reset_arenas(gs);
}
void gs_reset_arenas(GaussianSplat* gs) {
    gs->render_arena[0].capacity = 0;
    gs->render_arena[1].capacity = 0;
    gs->active_arena = 1;
}
void gs_set_camera(GaussianSplat* gs, Camera* c) {
    assert((c->width & 0xF) == 0, "Width must be divisble by 16");
    assert((c->height & 0xF) == 0, "Height must be divisble by 16");

    gs->c = c;
    gs->num_tiles = c->width * c->height / (TILE_SIZE * TILE_SIZE);

    assert(gs->num_tiles <= 4096, "Can have at most 4096 tiles");
}
void gs_free_kernels() {
    kernel_free(&project_points_k);
    kernel_free(&sh_k);
    kernel_free(&bbox_k);
    kernel_free(&tile_k);
    kernel_free(&render_k);
    kernel_free(&scan_rot_k);
    kernel_free(&scan_sum_k);
}

void gs_read_ply(GaussianSplat* gs, uint32_t ply_cluster, uint32_t filesize,
        Vec3* center, float* radius) {

    uint8_t* data;
    fat_readfile_cluster(ply_cluster, &data);

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
    float mn[] = { 1e9, 1e9, 1e9 };
    float mx[] = { -1e9, -1e9, -1e9 };
    for (uint32_t i = 0; i < num_gaussians; i++) {
        Gaussian gaus;
        memcpy(&gaus, data + st, sizeof(Gaussian));

        gs->g.pos_x[i] = gaus.pos_x;
        gs->g.pos_y[i] = gaus.pos_y;
        gs->g.pos_z[i] = gaus.pos_z;

        x_sum += gs->g.pos_x[i];
        y_sum += gs->g.pos_y[i];
        z_sum += gs->g.pos_z[i];
        mn[0] = min(mn[0], gs->g.pos_x[i]);
        mn[1] = min(mn[1], gs->g.pos_y[i]);
        mn[2] = min(mn[2], gs->g.pos_z[i]);
        mx[0] = max(mx[0], gs->g.pos_x[i]);
        mx[1] = max(mx[1], gs->g.pos_y[i]);
        mx[2] = max(mx[2], gs->g.pos_z[i]);

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

#ifdef VERBOSE
        if ((i + 1) % 5000 == 0) {
            uart_puts("Loaded ");
            uart_putd(i + 1);
            uart_puts(" gaussians\n");
        }
#endif
    }

    center->x = x_sum / num_gaussians;
    center->y = y_sum / num_gaussians;
    center->z = z_sum / num_gaussians;
    *radius = 3.0f * max(max(mx[0] - mn[0], mx[1] - mn[1]), mx[2] - mn[2]);
}

void qpu_scan(GaussianSplat* gs, uint32_t* arr, uint32_t n) {
    assert((n & 0xF) == 0, "N must be divisble by 16 for scan");
    assert(n >= gs->num_qpus * SIMD_WIDTH, "N too small for QPU scan");

    kernel_reset_unifs(&scan_rot_k);

    for (uint32_t q = 0; q < gs->num_qpus; q++) {
        kernel_load_unif(&scan_rot_k, q, gs->num_qpus * SIMD_WIDTH);
        kernel_load_unif(&scan_rot_k, q, n);
        kernel_load_unif(&scan_rot_k, q, q);

        kernel_load_unif(&scan_rot_k, q, TO_BUS(arr + q * SIMD_WIDTH));
    }

    kernel_execute(&scan_rot_k);

    uint32_t* pref = arena_alloc_align(&gs->render_arena[gs->active_arena], n * sizeof(uint32_t), 16 * sizeof(uint32_t));
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
}

void render_inactive_frame(GaussianSplat *gs) {
    uint32_t inactive_arena = !gs->active_arena;
    if (gs->render_arena[inactive_arena].capacity) {
#ifdef VERBOSE
        uart_puts("RENDERING...\n");
#endif

        kernel_reset_unifs(&render_k);

        ProjectedGaussianPtr* pg_all = &gs->pg_all[inactive_arena];
        for (uint32_t q = 0; q < gs->num_qpus; q++) {
            kernel_load_unif(&render_k, q, gs->num_qpus);
            kernel_load_unif(&render_k, q, gs->num_tiles);
            kernel_load_unif(&render_k, q, q);

            kernel_load_unif(&render_k, q, gs->c->width / TILE_SIZE);
            kernel_load_unif(&render_k, q, 1.0 * TILE_SIZE / gs->c->width);

            kernel_load_unif(&render_k, q, TO_BUS(pg_all->cov2d_inv_x));
            kernel_load_unif(&render_k, q, TO_BUS(pg_all->cov2d_inv_y));
            kernel_load_unif(&render_k, q, TO_BUS(pg_all->cov2d_inv_z));
            kernel_load_unif(&render_k, q, TO_BUS(pg_all->opacity));
            kernel_load_unif(&render_k, q, TO_BUS(pg_all->screen_x));
            kernel_load_unif(&render_k, q, TO_BUS(pg_all->screen_y));
            kernel_load_unif(&render_k, q, TO_BUS(pg_all->color_r));
            kernel_load_unif(&render_k, q, TO_BUS(pg_all->color_g));
            kernel_load_unif(&render_k, q, TO_BUS(pg_all->color_b));

            kernel_load_unif(&render_k, q, TO_BUS(gs->gaussians_touched[inactive_arena]));
            kernel_load_unif(&render_k, q, TO_BUS(gs->framebuffer + inactive_arena * gs->num_tiles * TILE_SIZE * TILE_SIZE));
        }

        kernel_execute_async(&render_k);
    }
}
void finish_render(GaussianSplat* gs) {
    uint32_t inactive_arena = !gs->active_arena;

    kernel_wait(&render_k);
    arena_dealloc_to(&gs->render_arena[inactive_arena], 0);

    // set virtual offset
    mbox_framebuffer_set_virtual_offset(0, inactive_arena * gs->c->height);
}

void render_sort(GaussianSplat* gs) {
    // launch render for last frame
    render_inactive_frame(gs);

    // key = tile (12 bits) | ~(upper 20 bits of depth)
    // depth bits are flipped bc we want depth descending
    // since we only have 12 bits for tile, we can only have max 4096 tiles

    uint32_t active_arena = gs->active_arena;
    Arena* arena = &gs->render_arena[active_arena];
    ProjectedGaussianPtr* pg_all = &gs->pg_all[active_arena];


    uint32_t* temp_key = arena_alloc_align(arena, gs->num_intersections * sizeof(uint32_t), 16 * sizeof(uint32_t));
    uint32_t* temp_id = arena_alloc_align(arena, gs->num_intersections * sizeof(uint32_t), 16 * sizeof(uint32_t));

    uint32_t* cnt = arena_alloc_align(arena, (1 << 16) * sizeof(uint32_t), 16 * sizeof(uint32_t));
    uint32_t* cnt2 = arena_alloc_align(arena, (1 << 16) * sizeof(uint32_t), 16 * sizeof(uint32_t));
    memset(cnt, 0, (1 << 16) * sizeof(uint32_t));
    memset(cnt2, 0, (1 << 16) * sizeof(uint32_t));

    gs->gaussians_touched[active_arena] = arena_alloc_align(arena, (gs->num_tiles + 1) * sizeof(uint32_t), 16 * sizeof(uint32_t));
    memset(gs->gaussians_touched[active_arena], 0, (gs->num_tiles + 1) * sizeof(uint32_t));

    {   // pass 1
        for (uint32_t i = 0; i < gs->num_intersections; i++) {
            uint32_t key = pg_all->depth_key[i].key;

            cnt[key & 0xFFFF]++;
            gs->gaussians_touched[active_arena][(key >> 20) + 1]++;
        }

        for (int i = 1; i < (1 << 16); i++) {
            cnt[i] += cnt[i - 1];
        }

        for (int i = gs->num_intersections - 1; i >= 0; i--) {
            uint32_t key = pg_all->depth_key[i].key;
            int j = key & 0xFFFF;
            temp_key[--cnt[j]] = (key >>= 16);
            temp_id[cnt[j]] = pg_all->radius_id[i].id;
            cnt2[key]++;
        }
    }

    {   // pass 2: no need to copy key
        for (int i = 1; i < (1 << 16); i++) {
            cnt2[i] += cnt2[i - 1];
        }
        
        for (int i = gs->num_intersections - 1; i >= 0; i--) {
            uint32_t j = --cnt2[temp_key[i]];
            uint32_t id = temp_id[i];
            pg_all->screen_x[j] = gs->pg.screen_x[id];
            pg_all->screen_y[j] = gs->pg.screen_y[id];
            pg_all->cov2d_inv_x[j] = gs->pg.cov2d_inv_x[id];
            pg_all->cov2d_inv_y[j] = gs->pg.cov2d_inv_y[id];
            pg_all->cov2d_inv_z[j] = gs->pg.cov2d_inv_z[id];
            pg_all->color_r[j] = gs->pg.color_r[id];
            pg_all->color_g[j] = gs->pg.color_g[id];
            pg_all->color_b[j] = gs->pg.color_b[id];
            pg_all->opacity[j] = gs->pg.opacity[id];
        }
    }

    for (uint32_t i = 0; i < gs->num_tiles; i++) {
        gs->gaussians_touched[active_arena][i + 1] += gs->gaussians_touched[active_arena][i];
    }

    assert(gs->gaussians_touched[active_arena][gs->num_tiles] == gs->num_intersections, "Intersection mismatch");

    // wait for render to finish and clear inactive arena
    finish_render(gs);
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

    kernel_execute(&project_points_k);

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

    kernel_execute(&sh_k);
}

void count_intersections(GaussianSplat* gs) {
    uint32_t active_arena = gs->active_arena;

    gs->tiles_touched[active_arena] = arena_alloc_align(&gs->render_arena[active_arena], (gs->num_gaussians + 1) * sizeof(uint32_t), 16 * sizeof(uint32_t));
    memset(gs->tiles_touched[active_arena], 0, (gs->num_gaussians + 1) * sizeof(uint32_t));

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

        kernel_load_unif(&bbox_k, q, TO_BUS(gs->tiles_touched[active_arena] + 1 + q * SIMD_WIDTH));
    }

    kernel_execute(&bbox_k);

    qpu_scan(gs, gs->tiles_touched[active_arena], (gs->num_gaussians + 1 + 15) & ~0xF);

    ///////// DUPLICATE GAUSSIANS + CALC TILE IDS

    gs->num_intersections = gs->tiles_touched[active_arena][gs->num_gaussians];
    assert(gs->num_intersections < MAX_GAUSSIANS, "Too many intersections");

    ProjectedGaussianPtr* pg_all = &gs->pg_all[active_arena];
    init_projected_gaussian_ptr(pg_all, &gs->render_arena[active_arena], gs->num_intersections);
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

        kernel_load_unif(&tile_k, q, TO_BUS(gs->tiles_touched[active_arena]));

        kernel_load_unif(&tile_k, q, TO_BUS(pg_all->depth_key + q * SIMD_WIDTH));
        kernel_load_unif(&tile_k, q, TO_BUS(pg_all->radius_id + q * SIMD_WIDTH));
    }

    kernel_execute(&tile_k);
}


void gs_render(GaussianSplat* gs) {
#ifdef VERBOSE
    uint32_t t;
    uart_puts("PREPROCESSING GAUSSIANS...\n");
    t = sys_timer_get_usec();
#endif

    precompute_gaussians_qpu(gs);

#ifdef VERBOSE
    uint32_t qpu_t = sys_timer_get_usec() - t;

    DEBUG_D(qpu_t);

    uart_puts("COUNTING INTERSECTIONS...\n");
#endif

    uint32_t active_arena = gs->active_arena;

    // initialize render arena if not already
    Arena* arena = &gs->render_arena[active_arena];
    if (!arena->capacity) {
        // allocate half of the remaining capacity
        uint32_t size = (gs->data_arena->capacity - gs->data_arena->size) / 2;
        uint8_t* buf = (uint8_t*) gs->data_arena->buf +
            gs->data_arena->size +
            (active_arena * size); // offset by size if active_arena = 1
        arena_init(arena, buf, size);
    }

    count_intersections(gs);

#ifdef VERBOSE
    uart_puts("SORTING...\n");

    t = sys_timer_get_usec();
#endif

    render_sort(gs);

#ifdef VERBOSE
    uint32_t qpu_sort_t = sys_timer_get_usec() - t;
    DEBUG_D(qpu_sort_t);

    uart_puts("Sort/render time: ");
    uart_putd(qpu_sort_t);
    uart_puts("\n");

    uart_puts("CPU clock: ");
    uart_putd(mbox_get_measured_clock_rate(MBOX_CLK_ARM));
    uart_puts("\n");

    uart_puts("GPU clock: ");
    uart_putd(mbox_get_measured_clock_rate(MBOX_CLK_V3D));
    uart_puts("\n");
#endif

    // flip active arena
    gs->active_arena ^= 1;
}
