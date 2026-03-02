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

// #define WIDTH 800
// #define HEIGHT 600
#define WIDTH 128
#define HEIGHT 96
#define TILE_SIZE 16
#define NUM_TILES (WIDTH * HEIGHT / (TILE_SIZE * TILE_SIZE))

#define NUM_QPUS 12
// #define NUM_QPUS 12
#define SIMD_WIDTH 16

// #define VERBOSE
// #define RENDER_CPU
#define RENDER_QPU

Arena data_arena;

uint32_t make_color(uint32_t r, uint32_t g, uint32_t b) {
    return (r << 16) | (g << 8) | b;
}

static uint8_t clamp_color(float c) {
    if (c < 0.0f) c = 0.0f;
    if (c > 1.0f) c = 1.0f;
    return (uint8_t)(c * 255.0f);
}

static uint32_t rand_state = 12345;

static uint32_t rand_u32(void) {
    rand_state = rand_state * 1103515245 + 12345;
    return rand_state;
}

static float rand_float(float min, float max) {
    return min + (float)(rand_u32() & 0xFFFF) / 65535.0f * (max - min);
}

void copy_pg_SoA(ProjectedGaussianSoA* pg1, ProjectedGaussianSoA* pg2,
        int pg1_idx, int pg2_idx) {
    pg1->screen_x[pg1_idx] = pg2->screen_x[pg2_idx];
    pg1->screen_y[pg1_idx] = pg2->screen_y[pg2_idx];
    pg1->depth[pg1_idx] = pg2->depth[pg2_idx];
    pg1->cov2d_inv[pg1_idx] = pg2->cov2d_inv[pg2_idx];
    pg1->color[pg1_idx] = pg2->color[pg2_idx];
    pg1->opacity[pg1_idx] = pg2->opacity[pg2_idx];
    pg1->radius[pg1_idx] = pg2->radius[pg2_idx];
    pg1->tile[pg1_idx] = pg2->tile[pg2_idx];
}
void swap_pg_soa(ProjectedGaussianSoA* pg1, ProjectedGaussianSoA* pg2,
        int pg1_idx, int pg2_idx) {
    ProjectedGaussian tmp = {
        pg1->screen_x[pg1_idx],
        pg1->screen_y[pg1_idx],
        pg1->depth[pg1_idx],
        pg1->cov2d_inv[pg1_idx],
        pg1->color[pg1_idx],
        pg1->opacity[pg1_idx],
        pg1->radius[pg1_idx],
        pg1->tile[pg1_idx]
    };

    pg1->screen_x[pg1_idx] = pg2->screen_x[pg2_idx];
    pg1->screen_y[pg1_idx] = pg2->screen_y[pg2_idx];
    pg1->depth[pg1_idx] = pg2->depth[pg2_idx];
    pg1->cov2d_inv[pg1_idx] = pg2->cov2d_inv[pg2_idx];
    pg1->color[pg1_idx] = pg2->color[pg2_idx];
    pg1->opacity[pg1_idx] = pg2->opacity[pg2_idx];
    pg1->radius[pg1_idx] = pg2->radius[pg2_idx];
    pg1->tile[pg1_idx] = pg2->tile[pg2_idx];

    pg2->screen_x[pg2_idx] = tmp.screen_x;
    pg2->screen_y[pg2_idx] = tmp.screen_y;
    pg2->depth[pg2_idx] = tmp.depth;
    pg2->cov2d_inv[pg2_idx] = tmp.cov2d_inv;
    pg2->color[pg2_idx] = tmp.color;
    pg2->opacity[pg2_idx] = tmp.opacity;
    pg2->radius[pg2_idx] = tmp.radius;
    pg2->tile[pg2_idx] = tmp.tile;
}

int cmp_less_SoA(ProjectedGaussianSoA* pg_soa, int a, int b) {
    if (pg_soa->tile[a] != pg_soa->tile[b]) {
        return pg_soa->tile[a] < pg_soa->tile[b];
    }
    return pg_soa->depth[a] > pg_soa->depth[b];
}
void sort_SoA(ProjectedGaussianSoA* pg_soa, int n) {
    ProjectedGaussianSoA temp;

    for (int b = 0; b < 32; b += 8) {
        uint32_t cnt[1 << 8];
        memset(cnt, 0, sizeof(cnt));
        for (int i = 0; i < n; i++) {
            uint32_t d = *((uint32_t*) &pg_soa->depth[i]);
            cnt[(d >> b) & 0xFF]++;
        }

        if (cnt[0] == n) {
            break;
        }

        for (int i = (1 << 8) - 2; i >= 0; i--) {
            cnt[i] += cnt[i + 1];
        }

        for (int i = n - 1; i >= 0; i--) {
            uint32_t d = *((uint32_t*) &pg_soa->depth[i]);
            int j = (d >> b) & 0xFF;
            copy_pg_SoA(&temp, pg_soa, cnt[j] - 1, i);
            cnt[j]--;
        }
        for (int i = 0; i < n; i++) {
            copy_pg_SoA(pg_soa, &temp, i, i);
        }
    }

    uint32_t tile_count[NUM_TILES + 1];
    memset(tile_count, 0, sizeof(tile_count));
    for (int i = 0; i < n; i++) {
        tile_count[pg_soa->tile[i]]++;
        assert(pg_soa->depth[i] >= 0, "negative depth");
    }
    for (int i = 1; i < NUM_TILES; i++) {
        tile_count[i] += tile_count[i - 1];
    }
    for (int i = n - 1; i >= 0; i--) {
        copy_pg_SoA(&temp, pg_soa, tile_count[pg_soa->tile[i]] - 1, i);
        tile_count[pg_soa->tile[i]]--;
    }
    for (int i = 0; i < n; i++) {
        copy_pg_SoA(pg_soa, &temp, i, i);
    }
}
void sort(ProjectedGaussianPtr* pg, uint32_t n, ProjectedGaussianPtr* orig, uint32_t* gaussians_touched) {
    float* temp_depth = arena_alloc_align(&data_arena, n * sizeof(float), 16);
    float* temp_tile = arena_alloc_align(&data_arena, n * sizeof(float), 16);
    uint32_t* temp_radius = arena_alloc_align(&data_arena, n * sizeof(uint32_t), 16);

    for (int b = 0; b < 32; b += 8) {
        uint32_t cnt[1 << 8];
        memset(cnt, 0, sizeof(cnt));
        for (int i = 0; i < n; i++) {
            uint32_t d = *((uint32_t*) &pg->depth[i]);
            cnt[(d >> b) & 0xFF]++;
        }

        if (cnt[0] == n) {
            break;
        }

        for (int i = (1 << 8) - 2; i >= 0; i--) {
            cnt[i] += cnt[i + 1];
        }

        for (int i = n - 1; i >= 0; i--) {
            uint32_t d = *((uint32_t*) &pg->depth[i]);
            int j = (d >> b) & 0xFF;
            temp_depth[cnt[j] - 1] = pg->depth[i];
            temp_tile[cnt[j] - 1] = pg->tile[i];
            temp_radius[cnt[j] - 1] = pg->radius_id[i].id;

            cnt[j]--;
        }
        for (int i = 0; i < n; i++) {
            pg->depth[i] = temp_depth[i];
            pg->tile[i] = temp_tile[i];
            pg->radius_id[i].id = temp_radius[i];
        }
    }

    uint32_t cnt[NUM_TILES + 1];
    memset(cnt, 0, sizeof(cnt));
    for (int i = 0; i < n; i++) {
        cnt[pg->tile[i]]++;
        assert(pg->depth[i] >= 0, "negative depth");
    }
    for (int i = 1; i < NUM_TILES; i++) {
        cnt[i] += cnt[i - 1];
    }
    for (int i = n - 1; i >= 0; i--) {
        int j = pg->tile[i];
        temp_depth[cnt[j] - 1] = pg->depth[i];
        temp_tile[cnt[j] - 1] = pg->tile[i];
        temp_radius[cnt[j] - 1] = pg->radius_id[i].id;

        cnt[j]--;
    }

    for (int i = 0; i < n; i++) {
        pg->depth[i] = temp_depth[i];
        pg->tile[i] = temp_tile[i];
        pg->radius_id[i].id = temp_radius[i];

        uint32_t id = pg->radius_id[i].id;
        pg->screen_x[i] = orig->screen_x[id];
        pg->screen_y[i] = orig->screen_y[id];
        pg->depth[i] = orig->depth[id];
        pg->cov2d_inv_x[i] = orig->cov2d_inv_x[id];
        pg->cov2d_inv_y[i] = orig->cov2d_inv_y[id];
        pg->cov2d_inv_z[i] = orig->cov2d_inv_z[id];
        pg->color_r[i] = orig->color_r[id];
        pg->color_g[i] = orig->color_g[id];
        pg->color_b[i] = orig->color_b[id];
        pg->opacity[i] = orig->opacity[id];

        gaussians_touched[pg->tile[i] + 1]++;
    }

    for (int i = 0; i < NUM_TILES; i++) {
        gaussians_touched[i + 1] += gaussians_touched[i];
    }
}

void precompute_gaussians_SoA(Camera* c, GaussianSoA* g, ProjectedGaussianSoA* pg, uint32_t num_gaussians) {
    for (uint32_t i = 0; i < num_gaussians; i++) {
        project_point(c, g->pos[i], &pg->depth[i], &pg->screen_x[i], &pg->screen_y[i]);
        if (pg->depth[i] < CAMERA_ZNEAR || pg->depth[i] > CAMERA_ZFAR) {
            pg->radius[i] = 0.f;
            continue;
        }

        Mat3 cov3d = compute_cov3d(g->scale[i], g->rot[i]);
        Vec3 cov2d = project_cov2d(g->pos[i], cov3d, c->w2c, c->fx, c->fy);

        cov2d.x += 0.3f;
        cov2d.z += 0.3f;

        pg->cov2d_inv[i] = compute_cov2d_inverse(cov2d);
        pg->color[i] = eval_sh(g->pos[i], g->sh[i], c->pos);

        float mid = 0.5f * (cov2d.x + cov2d.z);
        float det = max(cov2d.x * cov2d.z - cov2d.y * cov2d.y, 1e-6f);
        float lambda1 = mid + sqrtf(max(0.1f, mid * mid - det));
        float lambda2 = mid - sqrtf(max(0.1f, mid * mid - det));
        float r = 3.5f * sqrtf(max(lambda1, lambda2));
        pg->radius[i] = r;
    }
}

uint32_t ll[20000], rr[20000], bb[20000], tt[20000];
void count_intersections_SoA(ProjectedGaussianSoA* pg, int n,
        uint32_t* tiles_touched, uint32_t* gaussians_touched) {
    for (int i = 0; i < n; i++) {
        if (pg->radius[i] == 0.0f) {
            tiles_touched[i + 1] += tiles_touched[i];
            pg->tile[i] = 0;
            continue;
        }

        float rad = pg->radius[i];
        int32_t l = ((int32_t) (max(0.f, pg->screen_x[i] - rad))) / TILE_SIZE;
        int32_t r = min(WIDTH / TILE_SIZE - 1, ((((int32_t) (pg->screen_x[i] + rad)) + TILE_SIZE - 1) / TILE_SIZE));
        int32_t t = ((int32_t) (max(0.f, pg->screen_y[i] - rad))) / TILE_SIZE;
        int32_t b = min(HEIGHT / TILE_SIZE - 1, ((((int32_t) (pg->screen_y[i] + rad)) + TILE_SIZE - 1) / TILE_SIZE));

        assert(r >= l, "r < l");
        assert(b >= t, "b < t");
        assert(l >= 0, "l is neg");
        assert(t >= 0, "t is neg");

        ll[i] = l;
        rr[i] = r;
        bb[i] = b;
        tt[i] = t;

        tiles_touched[i + 1] = (r - l + 1) * (b - t + 1);
        for (int j = l; j <= r; j++) {
            for (int k = t; k <= b; k++) {
                int idx = k * (WIDTH / TILE_SIZE) + j;
                if (idx >= NUM_TILES) {
                    DEBUG_D(WIDTH / TILE_SIZE);
                    DEBUG_D(j);
                    DEBUG_D(HEIGHT / TILE_SIZE);
                    DEBUG_D(k);
                    DEBUG_D(idx);
                    panic("noo");
                }
                gaussians_touched[k * (WIDTH / TILE_SIZE) + j + 1]++;
            }
        }

        tiles_touched[i + 1] += tiles_touched[i];

        pg->tile[i] = 0;
    }

    for (int i = 0; i < NUM_TILES; i++) {
        gaussians_touched[i + 1] += gaussians_touched[i];
    }
}
void fill_all_gaussians_SoA(ProjectedGaussianSoA* pg, int n,
        ProjectedGaussianSoA* all_gaussians, uint32_t* tiles_touched) {
    for (int i = 0; i < n; i++) {
        if (pg->radius[i] == 0.0f) {
            continue;
        }

        float rad = pg->radius[i];
        int32_t l = ((int32_t) (max(0.f, pg->screen_x[i] - rad))) / TILE_SIZE;
        int32_t r = min(WIDTH / TILE_SIZE - 1, ((((int32_t) (pg->screen_x[i] + rad)) + TILE_SIZE - 1) / TILE_SIZE));
        int32_t t = ((int32_t) (max(0.f, pg->screen_y[i] - rad))) / TILE_SIZE;
        int32_t b = min(HEIGHT / TILE_SIZE - 1, ((((int32_t) (pg->screen_y[i] + rad)) + TILE_SIZE - 1) / TILE_SIZE));

        for (int j = l; j <= r; j++) {
            for (int k = t; k <= b; k++) {
                int tile_idx = (k - t) * (r - l + 1) + (j - l);

                int idx = tiles_touched[i] + tile_idx;
                copy_pg_SoA(all_gaussians, pg, idx, i);
                all_gaussians->tile[idx] = k * (WIDTH / TILE_SIZE) + j;
            }
        }
    }
}

void precompute_gaussians_qpu(Camera* c, GaussianPtr* g, ProjectedGaussianPtr* pg, uint32_t num_gaussians) {
    //////// PROJECT POINTS + COV2D INV

    Kernel project_points_k;
    const int project_points_num_unifs = 35;

    kernel_init(&project_points_k,
            NUM_QPUS, project_points_num_unifs,
            project_points_cov2d_inv, sizeof(project_points_cov2d_inv));

    for (uint32_t q = 0; q < NUM_QPUS; q++) {
        kernel_load_unif(&project_points_k, q, NUM_QPUS * SIMD_WIDTH);
        kernel_load_unif(&project_points_k, q, num_gaussians);
        kernel_load_unif(&project_points_k, q, q);
        kernel_load_unif(&project_points_k, q, TO_BUS(g->pos_x + q * SIMD_WIDTH));
        kernel_load_unif(&project_points_k, q, TO_BUS(g->pos_y + q * SIMD_WIDTH));
        kernel_load_unif(&project_points_k, q, TO_BUS(g->pos_z + q * SIMD_WIDTH));
        kernel_load_unif(&project_points_k, q, TO_BUS(pg->depth + q * SIMD_WIDTH));
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
    
    Kernel sh_k;
    const int sh_num_unifs = 60;
    kernel_init(&sh_k, NUM_QPUS, sh_num_unifs,
            spherical_harmonics, sizeof(spherical_harmonics));

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

uint32_t lll[20000], rrr[20000], bbb[20000], ttt[20000];
void count_intersections(ProjectedGaussianPtr* pg, int n,
        uint32_t* tiles_touched,
        ProjectedGaussianPtr* pg_all) {
    uint32_t t;

    /////////// CALCULATE BBOX

    Kernel bbox_k;
    const int bbox_num_unifs = 13;
    kernel_init(&bbox_k, NUM_QPUS, bbox_num_unifs,
            calc_bbox, sizeof(calc_bbox));

    uint32_t* lb = arena_alloc_align(&data_arena, n * sizeof(uint32_t), 16);
    uint32_t* rb = arena_alloc_align(&data_arena, n * sizeof(uint32_t), 16);
    uint32_t* tb = arena_alloc_align(&data_arena, n * sizeof(uint32_t), 16);
    uint32_t* bb = arena_alloc_align(&data_arena, n * sizeof(uint32_t), 16);
    for (uint32_t q = 0; q < NUM_QPUS; q++) {
        kernel_load_unif(&bbox_k, q, NUM_QPUS * SIMD_WIDTH);
        kernel_load_unif(&bbox_k, q, n);
        kernel_load_unif(&bbox_k, q, q);

        kernel_load_unif(&bbox_k, q, WIDTH / TILE_SIZE - 1);
        kernel_load_unif(&bbox_k, q, HEIGHT / TILE_SIZE - 1);

        kernel_load_unif(&bbox_k, q, TO_BUS(pg->screen_x + q * SIMD_WIDTH));
        kernel_load_unif(&bbox_k, q, TO_BUS(pg->screen_y + q * SIMD_WIDTH));
        kernel_load_unif(&bbox_k, q, TO_BUS(pg->radius_id + q * SIMD_WIDTH));

        kernel_load_unif(&bbox_k, q, TO_BUS(lb + q * SIMD_WIDTH));
        kernel_load_unif(&bbox_k, q, TO_BUS(rb + q * SIMD_WIDTH));
        kernel_load_unif(&bbox_k, q, TO_BUS(tb + q * SIMD_WIDTH));
        kernel_load_unif(&bbox_k, q, TO_BUS(bb + q * SIMD_WIDTH));
        kernel_load_unif(&bbox_k, q, TO_BUS(tiles_touched + 1 + q * SIMD_WIDTH));
    }

    t = sys_timer_get_usec();
    kernel_execute(&bbox_k);
    uint32_t bbox_kernel_t = sys_timer_get_usec() - t;
    DEBUG_D(bbox_kernel_t);

    kernel_free(&bbox_k);

    for (int i = 0; i < n; i++) {
        tiles_touched[i + 1] += tiles_touched[i];
        lll[i] = lb[i];
        rrr[i] = rb[i];
        ttt[i] = tb[i];
        bbb[i] = bb[i];
    }

    ///////// DUPLICATE GAUSSIANS + CALC TILE IDS

    uint32_t num_intersections = tiles_touched[n];
    init_projected_gaussian_ptr(pg_all, &data_arena, num_intersections);

    Kernel tile_k;
    const int tile_num_unifs = 14;
    kernel_init(&tile_k, NUM_QPUS, tile_num_unifs,
            calc_tile, sizeof(calc_tile));

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
        kernel_load_unif(&tile_k, q, TO_BUS(pg->depth));

        kernel_load_unif(&tile_k, q, TO_BUS(tiles_touched));

        kernel_load_unif(&tile_k, q, TO_BUS(pg_all->tile + q * SIMD_WIDTH));
        kernel_load_unif(&tile_k, q, TO_BUS(pg_all->depth + q * SIMD_WIDTH));
        // use radius as original Gaussian index bc we don't need it anymore for rendering
        kernel_load_unif(&tile_k, q, TO_BUS(pg_all->radius_id + q * SIMD_WIDTH));
    }

    t = sys_timer_get_usec();
    kernel_execute(&tile_k);
    uint32_t tile_kernel_t = sys_timer_get_usec() - t;
    DEBUG_D(tile_kernel_t);

    kernel_free(&tile_k);

    /*
    for (int i = 0; i < n; i++) {
        DEBUG_DM(i, "iteration");
        DEBUG_D(tiles_touched[i + 1] - tiles_touched[i]);
        if (i > 0) {
            DEBUG_D(pg_all->tile[tiles_touched[i] - 1]);
            DEBUG_D(pg_all->radius_id[tiles_touched[i] - 1].id);
        }
        DEBUG_D(pg_all->tile[tiles_touched[i]]);
        DEBUG_D(pg_all->radius_id[tiles_touched[i]].id);
        DEBUG_D(pg_all->tile[tiles_touched[i] + 1]);
        DEBUG_D(pg_all->radius_id[tiles_touched[i] + 1].id);
    }
    */
    /*
    for (int pp = 0; pp < 1; pp++) {
        DEBUG_D(pp);
        DEBUG_D(lb[pp]);
        DEBUG_D(rb[pp]);
        DEBUG_D(tb[pp]);
        DEBUG_D(bb[pp]);
        DEBUG_F(pg->depth[pp]);
        for (int i = lb[pp]; i <= rb[pp]; i++) {
            for (int j = tb[pp]; j <= bb[pp]; j++) {
                DEBUG_D(i * (HEIGHT / TILE_SIZE) + j);
            }
        }
        for (int i = tiles_touched[pp]; i < tiles_touched[pp + 1]; i++) {
            DEBUG_D(pg_all->tile[i]);
            DEBUG_D(pg_all->radius_id[i].id);
            DEBUG_F(pg_all->depth[i]);
        }
    }

    rpi_reset();
    */
}

void main() {
    // const int num_gaussians = NUM_QPUS * SIMD_WIDTH * 20;
    const int num_gaussians = NUM_QPUS * SIMD_WIDTH * 1;
    const int MiB = 1024 * 1024;
    arena_init(&data_arena, MiB * 35);

    uint32_t t;

    Vec3 cam_pos = { { 0.0f, 0.0f, 5.0f } };
    // Vec3 cam_pos = { { 0.0f, 0.0f, 100.f } };
    Vec3 cam_target = { { 0.0f, 0.0f, 0.0f } };
    Vec3 cam_up = { { 0.0f, 1.0f, 0.0f } };

    Camera* c = arena_alloc_align(&data_arena, sizeof(Camera), 16);
    init_camera(c, cam_pos, cam_target, cam_up, WIDTH, HEIGHT);

    GaussianPtr g;
    init_gaussian_ptr(&g, &data_arena, num_gaussians);

    ProjectedGaussianPtr pg;
    init_projected_gaussian_ptr(&pg, &data_arena, num_gaussians);

    GaussianSoA g_soa;
    ProjectedGaussianSoA pg_soa;

    for (int i = 0; i < num_gaussians; i++) {
        g_soa.pos[i] = (Vec3) { {
            rand_float(-2.0f, 2.0f),
            rand_float(-1.5f, 1.5f),
            rand_float(-2.0f, 2.0f)
        } };

        g.pos_x[i] = g_soa.pos[i].x;
        g.pos_y[i] = g_soa.pos[i].y;
        g.pos_z[i] = g_soa.pos[i].z;
        
        float scale = rand_float(-2.0f, -0.5f);
        g_soa.scale[i] = (Vec3) { { scale, scale, scale } };
        g_soa.rot[i] = (Vec4) { { 0.0f, 0.0f, 0.0f, 1.0f } };

        Mat3 cov3d_m = compute_cov3d(g_soa.scale[i], g_soa.rot[i]);

        g.cov3d[0][i] = cov3d_m.m[0];
        g.cov3d[1][i] = cov3d_m.m[1];
        g.cov3d[2][i] = cov3d_m.m[2];
        g.cov3d[3][i] = cov3d_m.m[4];
        g.cov3d[4][i] = cov3d_m.m[5];
        g.cov3d[5][i] = cov3d_m.m[8];

        pg_soa.opacity[i] = rand_float(0.5f, 1.0f); // load opacity directly to projected Gaussian
        pg.opacity[i] = pg_soa.opacity[i];
        
        for (int j = 0; j < 16; j++) {
            g_soa.sh[i][j] = (Vec3) { { 0.0f, 0.0f, 0.0f } };

            g.sh_x[j][i] = g_soa.sh[i][j].x;
            g.sh_y[j][i] = g_soa.sh[i][j].y;
            g.sh_z[j][i] = g_soa.sh[i][j].z;
        }
        
        float r = rand_float(0.2f, 1.0f);
        float gr = rand_float(0.2f, 1.0f);
        float b = rand_float(0.2f, 1.0f);
        g_soa.sh[i][0] = (Vec3) { {
            (r - 0.5f) / SH_C0,
            (gr - 0.5f) / SH_C0,
            (b - 0.5f) / SH_C0
        } };

        g.sh_x[0][i] = g_soa.sh[i][0].x;
        g.sh_y[0][i] = g_soa.sh[i][0].y;
        g.sh_z[0][i] = g_soa.sh[i][0].z;
    }

    DEBUG_D(num_gaussians);

    uart_puts("DONE LOADING GAUSSIANS\n");

    t = sys_timer_get_usec();
    precompute_gaussians_SoA(c, &g_soa, &pg_soa, num_gaussians);
    uint32_t cpu_t = sys_timer_get_usec() - t;
    DEBUG_D(cpu_t);

    t = sys_timer_get_usec();
    precompute_gaussians_qpu(c, &g, &pg, num_gaussians);
    uint32_t qpu_t = sys_timer_get_usec() - t;
    DEBUG_D(qpu_t);

    uart_puts("COUNTING INTERSECTIONS...\n");

    // cpu
    uint32_t tiles_touched_cpu[MAX_GAUSSIANS + 1];
    uint32_t gaussians_touched_cpu[NUM_TILES + 1];
    memset(tiles_touched_cpu, 0, (num_gaussians + 1) * sizeof(uint32_t));
    memset(gaussians_touched_cpu, 0, sizeof(gaussians_touched_cpu));
    count_intersections_SoA(&pg_soa, num_gaussians,
            tiles_touched_cpu, gaussians_touched_cpu);

    // qpu
    uint32_t* tiles_touched = arena_alloc_align(&data_arena, (num_gaussians + 1) * sizeof(uint32_t), 16);
    uint32_t* gaussians_touched = arena_alloc_align(&data_arena, (NUM_TILES + 1) * sizeof(uint32_t), 16);
    memset(tiles_touched, 0, (num_gaussians + 1) * sizeof(uint32_t));
    memset(gaussians_touched, 0, (NUM_TILES + 1) * sizeof(uint32_t));

#ifdef VERBOSE
    for (uint32_t i = 0; i < 16; i++) {
        uart_putd(i);
        uart_puts("\n");
        DEBUG_F(pg_soa.screen_x[i]);
        DEBUG_F(pg_soa.screen_y[i]);
        DEBUG_F(pg_soa.depth[i]);
        DEBUG_F(pg_soa.cov2d_inv[i].x);
        DEBUG_F(pg_soa.cov2d_inv[i].y);
        DEBUG_F(pg_soa.cov2d_inv[i].z);
        DEBUG_F(pg_soa.radius[i]);

        uart_puts("--\n");

        DEBUG_F(pg.screen_x[i]);
        DEBUG_F(pg.screen_y[i]);
        DEBUG_F(pg.depth[i]);
        DEBUG_F(pg.cov2d_inv_x[i]);
        DEBUG_F(pg.cov2d_inv_y[i]);
        DEBUG_F(pg.cov2d_inv_z[i]);
        DEBUG_F(pg.radius_id[i].radius);

        uart_puts("------\n");
    }
#endif

    ProjectedGaussianPtr pg_all;
    count_intersections(&pg, num_gaussians,
            tiles_touched, &pg_all);

    uint32_t total_intersections = tiles_touched_cpu[num_gaussians];
    DEBUG_D(total_intersections);
    DEBUG_D(gaussians_touched_cpu[NUM_TILES]);
    assert(total_intersections == gaussians_touched_cpu[NUM_TILES], "tile count mismatch");
    assert(total_intersections < MAX_GAUSSIANS, "too many intersections");

    DEBUG_D(tiles_touched[num_gaussians]);

    // /*
    for (int i = 0; i < num_gaussians; i++) {
        if (tiles_touched_cpu[i] != tiles_touched[i]) {
            DEBUG_DM(i, "iteration");
            DEBUG_D(tiles_touched_cpu[i]);
            DEBUG_D(tiles_touched[i]);

            DEBUG_D(ll[i - 1]);
            DEBUG_D(lll[i - 1]);
            DEBUG_D(rr[i - 1]);
            DEBUG_D(rrr[i - 1]);
            DEBUG_D(bb[i - 1]);
            DEBUG_D(bbb[i - 1]);
            DEBUG_D(tt[i - 1]);
            DEBUG_D(ttt[i - 1]);
            rpi_reset();
        }
    }
    // */

    assert(total_intersections == tiles_touched[num_gaussians], "cpu/qpu tile count mismatch");

    // rpi_reset();

    ProjectedGaussianSoA pg_soa_all;
    fill_all_gaussians_SoA(&pg_soa, num_gaussians, &pg_soa_all, tiles_touched_cpu);


    uart_puts("SORTING...\n");

    t = sys_timer_get_usec();
    sort_SoA(&pg_soa_all, total_intersections);
    uint32_t cpu_sort_t = sys_timer_get_usec() - t;
    DEBUG_D(cpu_sort_t);

    t = sys_timer_get_usec();
    sort(&pg_all, total_intersections, &pg, gaussians_touched);
    uint32_t qpu_sort_t = sys_timer_get_usec() - t;
    DEBUG_D(qpu_sort_t);

    /*
    for (int i = 0; i < NUM_TILES; i++) {
        if (gaussians_touched[i] != gaussians_touched_cpu[i]) {
            DEBUG_DM(i, "iter");
            DEBUG_D(gaussians_touched[i]);
            DEBUG_D(gaussians_touched_cpu[i]);
        }
    }
    */
    /*
    for (int i = 0; i < total_intersections; i++) {
        if (pg_all.tile[i] != pg_soa_all.tile[i]) {
            DEBUG_DM(i, "iter");
            DEBUG_D(pg_all.tile[i]);
            DEBUG_D(pg_soa_all.tile[i]);

            uint32_t id = pg_all.radius_id[i].id;
            DEBUG_D(ll[id - 1]);
            DEBUG_D(lll[id - 1]);
            DEBUG_D(rr[id - 1]);
            DEBUG_D(rrr[id - 1]);
            DEBUG_D(bb[id - 1]);
            DEBUG_D(bbb[id - 1]);
            DEBUG_D(tt[id - 1]);
            DEBUG_D(ttt[id - 1]);

            rpi_reset();
        }
    }
    */

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

#ifdef RENDER_CPU

    uart_puts("RENDERING\n");

    /*
    uart_puts("P3\n");
    uart_putd(WIDTH);
    uart_puts(" ");
    uart_putd(HEIGHT);
    uart_puts("\n255\n");
    */

    // CPU RENDER
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            float px = (float)x + 0.5f;
            float py = (float)y + 0.5f;

            float out_r = 20.0f / 255.0f;
            float out_g = 20.0f / 255.0f;
            float out_b = 40.0f / 255.0f;

            uint32_t tile_idx = (y / TILE_SIZE) * (WIDTH / TILE_SIZE) + (x / TILE_SIZE);
            for (uint32_t i = gaussians_touched_cpu[tile_idx]; i < gaussians_touched_cpu[tile_idx + 1]; i++) {
                assert(pg_all.depth[i] >= 0.0f, "negative depth");

                float alpha = pg_soa_all.opacity[i] * eval_gaussian_2d(px, py, pg_soa_all.screen_x[i], pg_soa_all.screen_y[i], pg_soa_all.cov2d_inv[i]);
                if (alpha < 0.001f) continue;

                out_r = alpha * pg_soa_all.color[i].x + (1.0f - alpha) * out_r;
                out_g = alpha * pg_soa_all.color[i].y + (1.0f - alpha) * out_g;
                out_b = alpha * pg_soa_all.color[i].z + (1.0f - alpha) * out_b;
            }

            pixels[y * WIDTH + x] = make_color(clamp_color(out_r), clamp_color(out_g), clamp_color(out_b));
        }
    }
    while (1);
#endif

#ifdef RENDER_QPU

    uart_puts("RENDERING\n");

    /*
    uart_puts("P3\n");
    uart_putd(WIDTH);
    uart_puts(" ");
    uart_putd(HEIGHT);
    uart_puts("\n255\n");
    */

    // QPU RENDER
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            float px = (float)x + 0.5f;
            float py = (float)y + 0.5f;

            float out_r = 20.0f / 255.0f;
            float out_g = 20.0f / 255.0f;
            float out_b = 40.0f / 255.0f;

            uint32_t tile_idx = (y / TILE_SIZE) * (WIDTH / TILE_SIZE) + (x / TILE_SIZE);
            for (uint32_t i = gaussians_touched[tile_idx]; i < gaussians_touched[tile_idx + 1]; i++) {
                assert(pg_all.depth[i] >= 0.0f, "negative depth");

                Vec3 cov2d_inv = { { pg_all.cov2d_inv_x[i], pg_all.cov2d_inv_y[i], pg_all.cov2d_inv_z[i] } };
                float alpha = pg_all.opacity[i] * eval_gaussian_2d(px, py, pg_all.screen_x[i], pg_all.screen_y[i], cov2d_inv);
                if (alpha < 0.001f) continue;

                out_r = alpha * pg_all.color_r[i] + (1.0f - alpha) * out_r;
                out_g = alpha * pg_all.color_g[i] + (1.0f - alpha) * out_g;
                out_b = alpha * pg_all.color_b[i] + (1.0f - alpha) * out_b;
            }

            pixels[y * WIDTH + x] = make_color(clamp_color(out_r), clamp_color(out_g), clamp_color(out_b));
            /*
            uart_putd(clamp_color(out_r));
            uart_puts(" ");
            uart_putd(clamp_color(out_g));
            uart_puts(" ");
            uart_putd(clamp_color(out_b));
            uart_puts("\n");
            */
        }
    }
    while (1);
#endif

    rpi_reset();
}
