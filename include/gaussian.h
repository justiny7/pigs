#ifndef GAUSSIAN_H
#define GAUSSIAN_H

#include "types.h"
#include "camera.h"
#include "arena_allocator.h"
#include <stdint.h>

// #define MAX_GAUSSIANS 96000
#define MAX_GAUSSIANS 4000000

// SH coefficients as static const to avoid QEMU .rodata loading issues
static const float SH_C0 = 0.28209479177387814f;
static const float SH_C1 = 0.4886025119029199f;

static const float SH_C2[] = {
    1.0925484305920792f,
    -1.0925484305920792f,
    0.31539156525252005f,
    -1.0925484305920792f,
    0.5462742152960396f
};
static const float SH_C3[] = {
    -0.5900435899266435f,
    2.890611442640554f,
    -0.4570457994644658f,
    0.3731763325901154f,
    -0.4570457994644658f,
    1.445305721320277f,
    -0.5900435899266435f
};

/*
typedef struct {
    Vec3 pos;
    Vec3 scale;
    Vec4 rot;
    float opacity;
    Vec3 sh[16];
} Gaussian;
*/

typedef struct __attribute__((packed, aligned(4))) {
    float pos_x;
    float pos_y;
    float pos_z;
    float f_dc[3];
    float f_rest[45];
    float opacity;
    float scale_x;
    float scale_y;
    float scale_z;
    float rot_w;
    float rot_x;
    float rot_y;
    float rot_z;
} Gaussian;

typedef struct {
    Vec3 pos[MAX_GAUSSIANS];
    Vec3 scale[MAX_GAUSSIANS];
    Vec4 rot[MAX_GAUSSIANS];
    Vec3 sh[MAX_GAUSSIANS][16];
} GaussianSoA;

typedef struct {
    float* pos_x;
    float* pos_y;
    float* pos_z;
    float* cov3d[6];
    float* sh_x[16];
    float* sh_y[16];
    float* sh_z[16];

    uint32_t size;
} GaussianPtr;

typedef struct {
    float screen_x, screen_y;
    float depth;
    Vec3 cov2d_inv;
    Vec3 color;
    float opacity;
    float radius;
    uint32_t tile;
} ProjectedGaussian;

typedef struct {
    float screen_x[MAX_GAUSSIANS];
    float screen_y[MAX_GAUSSIANS];
    float depth[MAX_GAUSSIANS];
    Vec3 cov2d_inv[MAX_GAUSSIANS];
    Vec3 color[MAX_GAUSSIANS];
    float opacity[MAX_GAUSSIANS];
    float radius[MAX_GAUSSIANS];
    uint32_t tile[MAX_GAUSSIANS];
} ProjectedGaussianSoA;

typedef struct {
    float* screen_x;
    float* screen_y;
    union { float depth; uint32_t key; }* depth_key;
    float* cov2d_inv_x;
    float* cov2d_inv_y;
    float* cov2d_inv_z;
    float* color_r;
    float* color_g;
    float* color_b;
    float* opacity;
    union { float radius; uint32_t id; }* radius_id;

    uint32_t size;
} ProjectedGaussianPtr;

void init_gaussian_ptr(GaussianPtr *p, Arena* a, uint32_t size);
void init_projected_gaussian_ptr(ProjectedGaussianPtr *p, Arena* a, uint32_t size);

Vec3 eval_sh(Vec3 pos, Vec3* sh, Vec3 cam_pos);
Mat3 compute_cov3d(Vec3 scale, Vec4 rot);
Vec3 project_cov2d(Vec3 pos, Mat3 cov3d, Mat4 w2c, float fx, float fy);
Vec3 compute_cov2d_inverse(Vec3 cov2d);
float eval_gaussian_2d(float px, float py, float cx, float cy, Vec3 cov_inv);

void precompute_gaussians(Camera* c, Gaussian* g, ProjectedGaussian* pg, uint32_t num_gaussians);

#endif
