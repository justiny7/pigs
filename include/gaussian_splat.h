#ifndef GAUSSIAN_SPLAT_H
#define GAUSSIAN_SPLAT_H

#include "arena_allocator.h"
#include "gaussian.h"
#include "kernel.h"

#define SIMD_WIDTH 16
#define TILE_SIZE 16

typedef struct {
    GaussianPtr g;
    ProjectedGaussianPtr pg, pg_all;

    Arena* data_arena;
    Camera* c;
    uint32_t* framebuffer;

    uint32_t num_gaussians, num_intersections;
    uint32_t num_tiles, num_qpus;
} GaussianSplat;

void gs_init(GaussianSplat* gs, Arena* data_arena, uint32_t* framebuffer, uint32_t num_qpus);
void gs_free(GaussianSplat* gs);

void gs_read_ply(GaussianSplat* gs, const char* filename,
        float* x_avg, float* y_avg, float* z_avg);
void gs_render(GaussianSplat* gs);
void gs_set_camera(GaussianSplat* gs, Camera* c);

#endif
