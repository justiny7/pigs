#ifndef GAUSSIAN_SPLAT_H
#define GAUSSIAN_SPLAT_H

#include "arena_allocator.h"
#include "gaussian.h"
#include "kernel.h"

#define SIMD_WIDTH 16
#define TILE_SIZE 16

typedef struct {
    GaussianPtr g;
    ProjectedGaussianPtr pg;
    ProjectedGaussianPtr pg_all[2];

    Arena* data_arena;
    Camera* c;
    uint32_t* framebuffer;

    uint32_t num_gaussians, num_intersections;
    uint32_t num_tiles, num_qpus;

    Arena render_arena[2]; // sub-arenas for current/next frame
    uint32_t active_arena;
    uint32_t* tiles_touched[2];
    uint32_t* gaussians_touched[2];
} GaussianSplat;

void gs_init(GaussianSplat* gs, Arena* data_arena, uint32_t* framebuffer, uint32_t num_qpus);
void gs_reset_arenas(GaussianSplat* gs);
void gs_free_kernels();

void gs_read_ply(GaussianSplat* gs, uint32_t ply_cluster, uint32_t filesize,
        Vec3* center, float* radius);
void gs_render(GaussianSplat* gs);
void gs_set_camera(GaussianSplat* gs, Camera* c);

#endif
